#include "Tpdd.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace tpdd;
namespace fs=std::filesystem;
int checks=0;
void check(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::vector<uint8_t> dirData(std::string name,uint8_t action=0) {
    std::vector<uint8_t> data(26,' ');
    std::copy(name.begin(),name.end(),data.begin()); data[24]='F'; data[25]=action;
    return data;
}
std::vector<uint8_t> ask(Drive& d,uint8_t cmd,std::vector<uint8_t> data={}) {
    return d.request(cmd,data.data(),data.size());
}
void code(const std::vector<uint8_t>& p,uint8_t expected) {
    check(p.size()==4 && p[0]==0x12 && p[1]==1 && p[2]==expected,"Unexpected status packet");
    unsigned sum=0; for (auto b:p) sum+=b;
    check((sum&255)==255,"Invalid response checksum");
}
void save(const fs::path& p,const std::string& s) { std::ofstream out(p,std::ios::binary); out<<s; }
std::string load(const fs::path& p) { std::ifstream in(p,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
int main() {
    char path[]="/tmp/tpdd-test-XXXXXX";
    auto* made=mkdtemp(path); if (!made) return 2;
    fs::path root(made), a=root/"a", b=root/"b";
    fs::create_directory(a); fs::create_directory(b);
    try {
        Drive d;
        std::string binary; for(int i=0;i<256;++i) binary+=char(i);
        save(a/"HELLO.DO",binary); save(b/"OTHER.DO","other bank");
        save(a/"LONGFILENAME.TXT","hidden");
        check(d.mount(0,a.string(),true),"Mount bank0");
        check(d.banks[0].skipped==1,"Unsupported name counted");
        check(!d.mount(1,a.string(),true),"Duplicate mount rejected");
        check(d.mount(1,b.string(),true),"Mount bank1");
        check(!d.mount(0,(root/"missing").string(),true),"Invalid mount rejected");
        check(d.banks[0].path==a.string(),"Failed mount preserved old bank");
        code(ask(d,7),0x70);
        check(d.setServing(true),"Start service");
        code(ask(d,7),0);
        auto list=ask(d,0,dirData("",1));
        check(list.size()==31 && list[0]==0x11,"Directory reply shape");
        check(std::string(list.begin()+2,list.begin()+11)=="HELLO .DO","6.2 filename padding");
        check(list[27]==1 && list[28]==0,"Big endian 256-byte length");
        auto missing=ask(d,0,dirData("NEW.DO"));
        check(missing[2]==0,"Missing selected file returns empty dirent");
        code(ask(d,1,{1}),0x50);
        auto selected=ask(d,0,dirData("HELLO .DO"));
        check(selected[2]=='H',"Padded request finds file");
        code(ask(d,1,{3}),0);
        check(!d.unmount(0) && !d.setServing(false) && !d.setReadOnly(0,false),"Open file blocks UI mutations");
        code(ask(d,0x43),0x35);
        auto first=ask(d,3),second=ask(d,3),end=ask(d,3);
        check(first.size()==131 && second.size()==131 && end.size()==3 && end[1]==0,"128 byte reads and empty EOF");
        std::string readback(first.begin()+2,first.end()-1); readback.append(second.begin()+2,second.end()-1);
        check(readback==binary,"Binary read preserved all byte values");
        code(ask(d,2),0);
        check(d.setReadOnly(0,false),"Enable writes");
        ask(d,0,dirData("NEW.DO")); code(ask(d,1,{1}),0);
        code(ask(d,4,{0,0xff,0x5a,13,10}),0); code(ask(d,2),0);
        check(load(a/"NEW.DO")==std::string("\0\xff\x5a\r\n",5),"Binary write preserved bytes");
        code(ask(d,1,{1}),0x11);
        code(ask(d,1,{2}),0); code(ask(d,4,{'!'}),0); code(ask(d,2),0);
        check(fs::file_size(a/"NEW.DO")==6,"Append writes at EOF");
        auto rename=dirData("REN.DO"); rename.resize(25);
        code(ask(d,0x0d,rename),0);
        check(!fs::exists(a/"NEW.DO") && fs::exists(a/"REN.DO"),"Rename isolated to bank0");
        code(ask(d,5),0); check(!fs::exists(a/"REN.DO"),"Delete removed selected file");
        code(ask(d,6),0x50); check(load(b/"OTHER.DO")=="other bank","Format cannot erase files");
        ask(d,0x40,dirData("OTHER.DO")); code(ask(d,0x41,{3}),0);
        auto bank1=ask(d,0x43); check(std::string(bank1.begin()+2,bank1.end()-1)=="other bank","Bank1 reads correct storage");
        code(ask(d,0x42),0);
        save(a/"LIMIT.DO",std::string(65534,'x'));
        ask(d,0,dirData("LIMIT.DO")); code(ask(d,1,{2}),0); code(ask(d,4,{'x'}),0x6e); code(ask(d,2),0);
        auto old=d.banks[0].path;
        save(b/"other.do","collision");
        check(!d.mount(0,b.string(),true),"Duplicate bank or collision rejected");
        check(!d.refresh(1) && d.error=="Client filename collision","Case-fold collision detected");
        fs::remove(b/"other.do");
        save(b/"HUGE.DO",std::string(65535,'x')); check(!d.refresh(1),"Oversize file rejected"); fs::remove(b/"HUGE.DO");
        check(d.refresh(1),"Directory recovers after errors");
        fs::create_directory_symlink(a,root/"link");
        check(!d.mount(1,(root/"link").string(),true),"Symlink mount rejected");
        fs::create_directory(a/"child");
        check(!d.mount(1,(root/"link"/"child").string(),true),"Symlink parent rejected");
        fs::path quota=root/"quota"; fs::create_directory(quota);
        save(quota/"ONE.DO",std::string(64000,'1'));
        save(quota/"TWO.DO",std::string(38400,'2'));
        check(d.mount(1,quota.string(),false),"Mount full virtual bank");
        ask(d,0x40,dirData("TWO.DO")); code(ask(d,0x41,{2}),0);
        code(ask(d,0x44,{'x'}),0x61); code(ask(d,0x42),0);
        check(fs::file_size(quota/"TWO.DO")==38400,"Quota rejection preserved data");
        fs::path many=root/"many"; fs::create_directory(many);
        for (unsigned i=0;i<41;++i) save(many/("F"+std::to_string(i)+".DO"),"x");
        check(!d.mount(1,many.string(),false),"Directory entry bound enforced");
        check(d.banks[1].path==quota.string(),"Failed large directory mount is atomic");
        check(d.mount(1,b.string(),true),"Restore bank1");
        ask(d,0x40,dirData("OTHER.DO"));
        code(ask(d,0x41,{2}),0x50); code(ask(d,0x45),0x50);
        auto roRename=dirData("CHANGED.DO"); roRename.resize(25);
        code(ask(d,0x4d,roRename),0x50);
        ask(d,0,dirData("PART.DO")); code(ask(d,1,{1}),0);
        code(ask(d,4,{'p'}),0);
        d.abortSession("Explicit test recovery");
        check(!d.busy() && !d.serving,"Recovery closes session and stops serving");
        check(load(a/"PART.DO")=="p","Recovery retains partial file for inspection");
        check(d.setServing(true),"Restart after recovery");
        save(a/"SHORT.DO","original data");
        ask(d,0,dirData("SHORT.DO")); code(ask(d,1,{3}),0);
        fs::resize_file(a/"SHORT.DO",0);
        code(ask(d,3),0x49);
        check(!d.busy() && !d.serving,"Unexpected short read cannot masquerade as successful EOF");
        check(d.setServing(true),"Restart after failed read");
        fs::remove(a/"SHORT.DO");
        code(ask(d,0,{}),0x36); code(ask(d,1,{}),0x36); code(ask(d,4,{}),0x36);
        code(ask(d,0x32),0x36);
        check(ask(d,0x23).size()==18,"TPDD2 version record");
        check(ask(d,0,dirData("",4)).empty(),"Close directory has no reply");
        code(ask(d,0,dirData("../BAD.DO")),0x30);
        check(!Drive::safePath("/a/../b") && !Drive::safePath("//b") && !Drive::safePath("/a/"),"Reject noncanonical paths");
        std::vector<std::vector<uint8_t>> sent;
        Parser parser(d,[&](const auto& p){sent.push_back(p);});
        uint32_t time=0;
        auto frame=packet(7,nullptr,0); frame.insert(frame.begin(),{0x5a,0x5a});
        for(auto c:frame) parser.feed(c,++time);
        check(sent.size()==1,"Fragmented byte stream parsed"); code(sent.back(),0);
        auto bad=frame; bad.back()^=1;
        for(auto c:bad) parser.feed(c,++time);
        check(sent.size()==1 && d.badFrames==1,"Bad checksum has no side effects or response");
        parser.feed(0x5a,++time); parser.feed(0x5a,++time); parser.feed(0,++time);
        parser.expire(time+1001); check(!parser.partial() && d.badFrames==2,"Truncated frame timeout");
        time+=1002;
        for(auto c:frame) parser.feed(c,++time);
        check(sent.size()==2,"Parser recovers after timeout");
        ask(d,0,dirData("BAD.DO")); code(ask(d,1,{1}),0);
        uint8_t payload=42; auto corrupt=packet(4,&payload,1);
        corrupt.back()^=1; corrupt.insert(corrupt.begin(),{0x5a,0x5a});
        for(auto c:corrupt) parser.feed(c,++time);
        code(ask(d,2),0);
        check(fs::file_size(a/"BAD.DO")==0,"Corrupt write frame cannot modify storage");
        // Entire 255-byte payload is consumed and rejected without overrunning.
        std::vector<uint8_t> large(255,0xaa); auto oversized=packet(4,large.data(),large.size());
        oversized.insert(oversized.begin(),{0x5a,0x5a});
        for(auto c:oversized) parser.feed(c,++time);
        code(sent.back(),0x36);
        // Exercise every command with short malformed payloads under ASan/UBSan.
        for(unsigned cmd=0;cmd<256;++cmd) for(unsigned n=0;n<4;++n) {
            std::vector<uint8_t> fuzz(n,0xff); ask(d,cmd,fuzz);
        }
        check(fs::exists(a/"HELLO.DO") && fs::exists(b/"OTHER.DO"),"Malformed commands preserve existing data");
        fs::remove_all(root);
        std::cout<<"PASS: "<<checks<<" checks; file operations, banks, safety, framing, malformed input\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<" (fixtures: "<<root<<")\n"; return 1;
    }
}
