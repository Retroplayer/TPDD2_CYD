#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <driver/uart.h>
#include <dirent.h>
#include <sys/stat.h>
#include "Tpdd.h"

namespace {
constexpr uart_port_t Port = UART_NUM_1;
constexpr int Rx = 2, Tx = 3, Rts = 14, Cts = 21;
constexpr int Rows = 3;
enum class Action : uint8_t { Browse, Mount, Unmount, ReadOnly, Serve, Media, Flow, Recover };
struct Command { Action action; uint32_t id; unsigned bank; bool value; unsigned page; char path[192]; };
struct Snapshot {
    char paths[2][192]{}, saved[2][192]{}, message[96]{}, activity[96]{};
    bool ro[2]{true,true}, mounted[2]{}, serving=false, busy=false, media=false, cts=true;
    uint32_t requests=0, bad=0, rxBytes=0, txBytes=0, uartErrors=0, lastRequest=0, ack=0;
    size_t files[2]{}, skipped[2]{};
    char folder[192]{"/"}, children[Rows][192]{}, names[Rows][64]{};
    unsigned page=0, count=0; bool next=false;
    size_t heap=0, psram=0;
};
QueueHandle_t commands, snapshots, uartEvents;
TFT_eSPI tft;
lv_disp_draw_buf_t drawBuffer;
lv_color_t pixels[320 * 20];
lv_obj_t *statusLabel=nullptr, *countLabel=nullptr, *bankLabels[2]{}, *messageLabel=nullptr;
lv_obj_t *serveButton=nullptr;
Snapshot current;
uint32_t nextCommand=0, pending=0;
unsigned pickerBank=0, pickerPage=0;
std::string pickerPath="/";
enum class Screen { Home, Picker, Settings, Recovery } screen=Screen::Home;
bool scanPending=false;
Preferences preferences;

void copyText(char* dst, size_t n, const std::string& src) { snprintf(dst,n,"%s",src.c_str()); }
std::string relative(const std::string& path) {
    return path == "/sdcard" ? "/" : path.substr(7);
}
bool initSd() {
    SD_MMC.setPins(38,40,39,41,48,47);
    return SD_MMC.begin("/sdcard",false,false,20000,5);
}
void initUart(bool cts) {
    digitalWrite(Rts,HIGH); pinMode(Rts,OUTPUT);
    digitalWrite(Tx,HIGH); pinMode(Tx,OUTPUT);
    uart_config_t config{};
    config.baud_rate=19200; config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE; config.stop_bits=UART_STOP_BITS_1;
    config.flow_ctrl=cts?UART_HW_FLOWCTRL_CTS_RTS:UART_HW_FLOWCTRL_RTS;
    config.rx_flow_ctrl_thresh=64; config.source_clk=UART_SCLK_APB;
    ESP_ERROR_CHECK(uart_param_config(Port,&config));
    ESP_ERROR_CHECK(uart_set_pin(Port,Tx,Rx,Rts,Cts));
    ESP_ERROR_CHECK(uart_driver_install(Port,2048,1024,16,&uartEvents,0));
}
void browse(Snapshot& out, const Command& cmd) {
    out.count=0; out.next=false; out.page=cmd.page;
    copyText(out.folder,sizeof(out.folder),cmd.path);
    for (int i=0;i<Rows;++i) { out.children[i][0]=0; out.names[i][0]=0; }
    std::string rel=cmd.path;
    if (!tpdd::Drive::safePath(rel)) { copyText(out.message,sizeof(out.message),"Invalid folder"); return; }
    auto base=std::string("/sdcard")+(rel=="/"?"":rel);
    DIR* dir=opendir(base.c_str());
    if (!dir) { copyText(out.message,sizeof(out.message),"Folder unavailable; try SD root"); return; }
    unsigned skipped=0;
    while (auto* item=readdir(dir)) {
        std::string name=item->d_name;
        if (name=="." || name=="..") continue;
        auto child=(rel=="/"?"":rel)+"/"+name;
        if (!tpdd::Drive::safePath(child) || child.size()+7>190) continue;
        struct stat st{};
        if (stat((std::string("/sdcard")+child).c_str(),&st) || !S_ISDIR(st.st_mode)) continue;
        if (skipped++ < cmd.page*Rows) continue;
        if (out.count==Rows) { out.next=true; break; }
        copyText(out.children[out.count],sizeof(out.children[0]),child);
        copyText(out.names[out.count],sizeof(out.names[0]),name);
        ++out.count;
    }
    closedir(dir);
    copyText(out.message,sizeof(out.message),out.count?"Select a folder or use the current one":"No subfolders; current folder can be used");
}
void worker(void*) {
    tpdd::Drive drive;
    Snapshot out;
    out.media=initSd();
    bool prefsReady=preferences.begin("tpdd2",false);
    for (unsigned b=0;b<2;++b) {
        String key=String("bank")+b;
        copyText(out.saved[b],sizeof(out.saved[b]),preferences.getString(key.c_str(),"/").c_str());
    }
    copyText(out.message,sizeof(out.message),out.media?"Choose folders; banks start read-only":"Insert FAT32 SD, then press Retry SD");
    tpdd::Parser parser(drive,[&](const std::vector<uint8_t>& packet) {
        uart_write_bytes(Port,packet.data(),packet.size());
    });
    uint32_t lastPublish=0;
    for (;;) {
        // One request/response at a time. Wait for CTS-gated TX to drain before
        // accepting another request or changing a mount.
        bool txIdle=uart_wait_tx_done(Port,0)==ESP_OK;
        uart_event_t event;
        while (xQueueReceive(uartEvents,&event,0)==pdTRUE) {
            if (event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL ||
                event.type==UART_FRAME_ERR || event.type==UART_PARITY_ERR) {
                ++out.uartErrors;
                uart_flush_input(Port); parser.reset();
                drive.abortSession("UART error; partial file may remain");
                copyText(out.message,sizeof(out.message),drive.error);
            }
        }
        if (txIdle) {
            uint8_t byte;
            for (unsigned i=0;i<260 && uart_read_bytes(Port,&byte,1,0)==1;++i) {
                uint32_t before=drive.requests;
                parser.feed(byte,millis());
                if (drive.requests!=before) { out.lastRequest=millis(); break; }
            }
        }
        parser.expire(millis());
        // Parsing above may have queued a response since the first TX check.
        txIdle=uart_wait_tx_done(Port,0)==ESP_OK;
        size_t rxPending=0;
        uart_get_buffered_data_len(Port,&rxPending);
        Command cmd{};
        bool recovery=xQueuePeek(commands,&cmd,0)==pdTRUE && cmd.action==Action::Recover;
        if ((recovery || (txIdle && !parser.partial() && !rxPending)) && xQueueReceive(commands,&cmd,0)==pdTRUE) {
            drive.error.clear();
            bool ok=true;
            if (cmd.action==Action::Recover) {
                drive.abortSession("Session reset; inspect partial files");
                parser.reset();
                ESP_ERROR_CHECK(uart_driver_delete(Port));
                initUart(out.cts); xQueueReset(commands);
                copyText(out.message,sizeof(out.message),drive.error);
            } else if (cmd.action==Action::Browse) {
                if (out.media) browse(out,cmd);
                else { out.count=0; copyText(out.message,sizeof(out.message),"SD not mounted"); }
            } else if (cmd.action==Action::Mount) {
                std::string rel=cmd.path;
                if (!out.media || !tpdd::Drive::safePath(rel)) {
                    ok=false; drive.error="SD or folder unavailable";
                } else {
                    ok=drive.mount(cmd.bank,"/sdcard"+(rel=="/"?std::string():rel),true);
                    if (ok) {
                        String key=String("bank")+cmd.bank;
                        bool saved=prefsReady && preferences.putString(key.c_str(),rel.c_str())==rel.size();
                        copyText(out.saved[cmd.bank],sizeof(out.saved[0]),rel);
                        snprintf(out.message,sizeof(out.message),"Mounted; %u unsupported files hidden",
                                 static_cast<unsigned>(drive.banks[cmd.bank].skipped));
                        if (!saved) copyText(out.message,sizeof(out.message),"Mounted; last-folder preference not saved");
                    }
                }
            } else if (cmd.action==Action::Unmount) ok=drive.unmount(cmd.bank);
            else if (cmd.action==Action::ReadOnly) ok=drive.setReadOnly(cmd.bank,cmd.value);
            else if (cmd.action==Action::Serve) ok=drive.setServing(cmd.value);
            else if (cmd.action==Action::Media) {
                if (drive.busy()) { ok=false; drive.error="Close client file first"; }
                else {
                    drive.setServing(false); drive.unmount(0); drive.unmount(1);
                    SD_MMC.end();
                    if (out.media) { out.media=false; copyText(out.message,sizeof(out.message),"SD ejected; safe to remove"); }
                    else { out.media=initSd(); copyText(out.message,sizeof(out.message),out.media?"SD ready; choose bank folders":"SD mount failed"); }
                }
            } else if (cmd.action==Action::Flow) {
                if (drive.serving || drive.busy()) { ok=false; drive.error="Stop service before changing CTS"; }
                else {
                    out.cts=cmd.value;
                    ESP_ERROR_CHECK(uart_set_hw_flow_ctrl(Port,out.cts?UART_HW_FLOWCTRL_CTS_RTS:UART_HW_FLOWCTRL_RTS,64));
                    copyText(out.message,sizeof(out.message),out.cts?"CTS enabled":"CTS OFF: diagnostic use only");
                }
            }
            if (!ok) copyText(out.message,sizeof(out.message),drive.error);
            else if (cmd.action==Action::Serve) copyText(out.message,sizeof(out.message),cmd.value?"Service started":"Service stopped");
            else if (cmd.action==Action::Unmount) copyText(out.message,sizeof(out.message),"Bank unmounted");
            else if (cmd.action==Action::ReadOnly) copyText(out.message,sizeof(out.message),cmd.value?"Bank read-only":"Bank writable; use test files");
            out.ack=cmd.id;
        }
        if (millis()-lastPublish>=100) {
            out.serving=drive.serving; out.busy=drive.busy();
            out.requests=drive.requests; out.bad=drive.badFrames;
            out.rxBytes=drive.bytesRead; out.txBytes=drive.bytesWritten;
            out.heap=ESP.getFreeHeap(); out.psram=ESP.getFreePsram();
            copyText(out.activity,sizeof(out.activity),drive.activity);
            if (!drive.error.empty()) copyText(out.message,sizeof(out.message),drive.error);
            for (unsigned b=0;b<2;++b) {
                const auto& bank=drive.banks[b];
                out.mounted[b]=!bank.path.empty(); out.ro[b]=bank.readOnly;
                copyText(out.paths[b],sizeof(out.paths[b]),bank.path.empty()?"Not mounted":relative(bank.path));
                out.files[b]=bank.files.size(); out.skipped[b]=bank.skipped;
            }
            xQueueOverwrite(snapshots,&out); lastPublish=millis();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
void flushDisplay(lv_disp_drv_t* driver,const lv_area_t* area,lv_color_t* colors) {
    tft.startWrite();
    tft.setAddrWindow(area->x1,area->y1,area->x2-area->x1+1,area->y2-area->y1+1);
    tft.pushColors(reinterpret_cast<uint16_t*>(colors),(area->x2-area->x1+1)*(area->y2-area->y1+1),true);
    tft.endWrite(); lv_disp_flush_ready(driver);
}
void readTouch(lv_indev_drv_t*,lv_indev_data_t* data) {
    // FT6336/FT5x06 register map: count at 0x02, first X/Y at 0x03..0x06.
    data->state=LV_INDEV_STATE_RELEASED;
    Wire.beginTransmission(0x38); Wire.write(0x02);
    if (Wire.endTransmission(false)!=0 || Wire.requestFrom(0x38,5)!=5) return;
    uint8_t count=Wire.read()&0x0f;
    uint16_t x=(Wire.read()&0x0f)<<8; x|=Wire.read();
    uint16_t y=(Wire.read()&0x0f)<<8; y|=Wire.read();
    if (!count || x>=240 || y>=320) return;
    data->point.x=y; data->point.y=239-x;
    data->state=LV_INDEV_STATE_PRESSED;
}
lv_obj_t* label(lv_obj_t* parent,const char* text,int x,int y,int width) {
    auto* obj=lv_label_create(parent); lv_obj_set_pos(obj,x,y); lv_obj_set_width(obj,width);
    lv_label_set_long_mode(obj,LV_LABEL_LONG_DOT); lv_label_set_text(obj,text); return obj;
}
lv_obj_t* button(const char* text,int x,int y,int w,int h,lv_event_cb_t cb,intptr_t value=0) {
    auto* obj=lv_btn_create(lv_scr_act()); lv_obj_set_pos(obj,x,y); lv_obj_set_size(obj,w,h);
    lv_obj_add_event_cb(obj,cb,LV_EVENT_CLICKED,reinterpret_cast<void*>(value));
    auto* textObj=lv_label_create(obj); lv_label_set_text(textObj,text); lv_obj_center(textObj); return obj;
}
void clearScreen(const char* title) {
    statusLabel=countLabel=messageLabel=serveButton=nullptr;
    bankLabels[0]=bankLabels[1]=nullptr;
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(),lv_color_hex(0x101923),0);
    lv_obj_set_style_text_color(lv_scr_act(),lv_color_hex(0xe5edf5),0);
    label(lv_scr_act(),title,8,5,304);
}
bool sendCommand(Action action,unsigned bank=0,bool value=false,const std::string& path="/",unsigned page=0) {
    if (pending && action!=Action::Recover) return false;
    Command cmd{}; cmd.action=action; cmd.id=++nextCommand; cmd.bank=bank; cmd.value=value; cmd.page=page;
    copyText(cmd.path,sizeof(cmd.path),path);
    BaseType_t queued = action==Action::Recover ? xQueueSendToFront(commands,&cmd,0) : xQueueSend(commands,&cmd,0);
    if (queued!=pdTRUE) return false;
    pending=cmd.id; return true;
}
void home(); void picker(); void settings();
void goHome(lv_event_t*) { scanPending=false; home(); }
void scanFolder() { scanPending=sendCommand(Action::Browse,pickerBank,false,pickerPath,pickerPage); }
void openPicker(lv_event_t* event) {
    if (pending) return;
    pickerBank=reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    pickerPath=current.mounted[pickerBank]?current.paths[pickerBank]:current.saved[pickerBank];
    if (pickerPath.empty()) pickerPath="/";
    pickerPage=0; picker(); scanFolder();
}
void chooseChild(lv_event_t* event) {
    if (pending) return;
    unsigned row=reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (row>=current.count) return;
    pickerPath=current.children[row]; pickerPage=0; scanFolder();
}
void navigate(lv_event_t* event) {
    if (pending) return;
    intptr_t direction=reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (!direction) { auto slash=pickerPath.find_last_of('/'); pickerPath=slash==0?"/":pickerPath.substr(0,slash); pickerPage=0; }
    else if (direction>0) ++pickerPage;
    else if (pickerPage) --pickerPage;
    scanFolder();
}
void useFolder(lv_event_t*) {
    if (pending) return;
    if (sendCommand(Action::Mount,pickerBank,true,pickerPath)) home();
}
void toggleService(lv_event_t*) { sendCommand(Action::Serve,0,!current.serving); }
void toggleRo(lv_event_t* event) {
    unsigned bank=reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    sendCommand(Action::ReadOnly,bank,!current.ro[bank]);
}
void unmount(lv_event_t* event) { sendCommand(Action::Unmount,reinterpret_cast<intptr_t>(lv_event_get_user_data(event))); }
void mediaAction(lv_event_t*) { sendCommand(Action::Media); }
void flowAction(lv_event_t*) { sendCommand(Action::Flow,0,!current.cts); }
void confirmRecovery(lv_event_t*) {
    screen=Screen::Recovery;
    clearScreen("Recover serial session?");
    auto* explanation=label(lv_scr_act(),"Stop the laptop first. This stops service, closes any open file and discards queued serial data. Partial files are kept; check their contents before reuse.",8,40,304);
    lv_label_set_long_mode(explanation,LV_LABEL_LONG_WRAP);
    button("Cancel",8,185,145,42,goHome);
    button("Stop + close",161,185,151,42,[](lv_event_t*){sendCommand(Action::Recover);home();});
}
void home() {
    screen=Screen::Home; clearScreen("TPDD2 r0.1  |  Model 100");
    for (unsigned b=0;b<2;++b) {
        int y=30+b*61;
        char text[200]; snprintf(text,sizeof(text),"Bank %u: %s",b,current.paths[b]);
        bankLabels[b]=label(lv_scr_act(),text,8,y,304);
        lv_label_set_long_mode(bankLabels[b],LV_LABEL_LONG_SCROLL_CIRCULAR);
        button("Folder",8,y+18,95,38,openPicker,b);
        button(current.ro[b]?"Read only":"Writable",110,y+18,99,38,toggleRo,b);
        button("Unmount",216,y+18,96,38,unmount,b);
    }
    serveButton=button(current.serving?"Stop":"Start",8,155,94,38,toggleService);
    button(current.media?"Eject SD":"Retry SD",110,155,99,38,mediaAction);
    button("Settings",216,155,96,38,[](lv_event_t*){ settings(); });
    statusLabel=label(lv_scr_act(),current.activity,8,197,304);
    messageLabel=label(lv_scr_act(),current.message,8,218,304);
}
void picker() {
    screen=Screen::Picker; clearScreen("Choose a folder");
    statusLabel=label(lv_scr_act(),pickerPath.c_str(),8,27,304);
    lv_label_set_long_mode(statusLabel,LV_LABEL_LONG_SCROLL_CIRCULAR);
    for (unsigned i=0;i<current.count && i<Rows;++i) button(current.names[i],8,50+i*37,304,34,chooseChild,i);
    if (!current.count) {
        auto* message=label(lv_scr_act(),pending?"Loading...":current.message,8,64,304);
        lv_label_set_long_mode(message,LV_LABEL_LONG_WRAP);
    }
    button("Up",8,165,72,32,navigate,0);
    if (pickerPage) button("Prev",86,165,72,32,navigate,-1);
    if (current.next) button("Next",164,165,72,32,navigate,1);
    button("Root",242,165,70,32,[](lv_event_t*){ if(!pending){pickerPath="/";pickerPage=0;scanFolder();} });
    button("Cancel",8,203,145,34,goHome);
    button("Use this folder",161,203,151,34,useFolder);
}
void settings() {
    screen=Screen::Settings; clearScreen("Settings / diagnostics");
    label(lv_scr_act(),"19200 8N1 | RX2 TX3 RTS14 CTS21",8,30,304);
    char info[180]; snprintf(info,sizeof(info),"Requests %lu | bad frames %lu\nRead %lu B | written %lu B\nUART errors %lu\nHeap %u | PSRAM free %u",(unsigned long)current.requests,(unsigned long)current.bad,(unsigned long)current.rxBytes,(unsigned long)current.txBytes,(unsigned long)current.uartErrors,(unsigned)current.heap,(unsigned)current.psram);
    countLabel=label(lv_scr_act(),info,8,57,304); lv_obj_set_height(countLabel,90);
    button(current.cts?"CTS ON":"CTS OFF (test)",8,155,145,38,flowAction);
    button("Recover",161,155,151,38,confirmRecovery);
    button("Back",216,202,96,32,goHome);
    messageLabel=label(lv_scr_act(),current.message,8,202,200);
}
void pollUi(lv_timer_t*) {
    Snapshot update;
    if (xQueueReceive(snapshots,&update,0)!=pdTRUE) return;
    bool ack=pending && update.ack>=pending;
    bool controls=update.serving!=current.serving || update.media!=current.media || update.ro[0]!=current.ro[0] || update.ro[1]!=current.ro[1] || update.cts!=current.cts;
    current=update;
    if (ack) {
        pending=0;
        if (scanPending && screen==Screen::Picker) { scanPending=false; picker(); }
        else if (screen==Screen::Home) home();
        else if (screen==Screen::Settings) settings();
    } else if (controls && screen==Screen::Home) home();
    if (screen==Screen::Home && statusLabel) {
        char activity[120];
        if (pending) snprintf(activity,sizeof(activity),"Waiting for serial transaction...");
        else if (current.busy) snprintf(activity,sizeof(activity),"%s (file open)",current.activity);
        else snprintf(activity,sizeof(activity),"%s | req %lu",current.activity,(unsigned long)current.requests);
        lv_label_set_text(statusLabel,activity);
    }
    if (messageLabel) lv_label_set_text(messageLabel,current.message);
    if (screen==Screen::Settings && countLabel) {
        char info[220];
        char age[32];
        if(current.requests) snprintf(age,sizeof(age),"%lus ago",(unsigned long)((millis()-current.lastRequest)/1000));
        else snprintf(age,sizeof(age),"none");
        snprintf(info,sizeof(info),"Requests %lu | bad %lu | UART %lu\nRead %lu B | written %lu B\nLast request: %s\nHeap %u | PSRAM %u",
                 (unsigned long)current.requests,(unsigned long)current.bad,(unsigned long)current.uartErrors,
                 (unsigned long)current.rxBytes,(unsigned long)current.txBytes,
                 age,
                 (unsigned)current.heap,(unsigned)current.psram);
        lv_label_set_text(countLabel,info);
    }
}
}
void setup() {
    // External 10k pull-ups on GPIO3/14 define TX idle and RTS stop at reset.
    Serial.begin(115200);
    initUart(true);
    commands=xQueueCreate(4,sizeof(Command)); snapshots=xQueueCreate(1,sizeof(Snapshot));
    configASSERT(commands && snapshots);
    Wire.begin(16,15,400000); Wire.setTimeOut(10);
    pinMode(18,OUTPUT); digitalWrite(18,LOW); delay(10); digitalWrite(18,HIGH); delay(150);
    tft.begin(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
    lv_init(); lv_disp_draw_buf_init(&drawBuffer,pixels,nullptr,320*20);
    static lv_disp_drv_t display; lv_disp_drv_init(&display);
    display.hor_res=320; display.ver_res=240; display.flush_cb=flushDisplay; display.draw_buf=&drawBuffer;
    lv_disp_drv_register(&display);
    static lv_indev_drv_t touch; lv_indev_drv_init(&touch);
    touch.type=LV_INDEV_TYPE_POINTER; touch.read_cb=readTouch; lv_indev_drv_register(&touch);
    configASSERT(xTaskCreatePinnedToCore(worker,"tpdd-owner",12288,nullptr,3,nullptr,0)==pdPASS);
    home(); lv_timer_create(pollUi,100,nullptr);
    Serial.printf("TPDD2 r0.1 | flash=%u PSRAM=%u heap=%u\n",ESP.getFlashChipSize(),ESP.getPsramSize(),ESP.getFreeHeap());
}
void loop() {
    lv_timer_handler();
    static uint32_t last=0;
    if (millis()-last>2000) {
        Serial.printf("req=%lu bad=%lu uart=%lu busy=%u serve=%u %s\n",(unsigned long)current.requests,(unsigned long)current.bad,(unsigned long)current.uartErrors,current.busy,current.serving,current.activity);
        last=millis();
    }
    delay(5);
}
