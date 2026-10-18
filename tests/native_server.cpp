#include "Tpdd.h"
#include <chrono>
#include <unistd.h>
int main(int argc,char** argv) {
    if (argc!=3) return 2;
    tpdd::Drive drive;
    if (!drive.mount(0,argv[1],false) || !drive.mount(1,argv[2],false)) return 3;
    drive.setServing(true);
    tpdd::Parser parser(drive,[](const auto& response) {
        size_t pos=0;
        while(pos<response.size()) {
            auto n=write(STDOUT_FILENO,response.data()+pos,response.size()-pos);
            if(n<=0) _exit(4);
            pos+=n;
        }
    });
    uint8_t byte;
    while (read(STDIN_FILENO,&byte,1)==1) {
        auto now=std::chrono::steady_clock::now().time_since_epoch();
        parser.feed(byte,std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }
}
