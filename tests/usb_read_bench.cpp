// SPDX-License-Identifier: GPL-2.0-only
// Matched Windows/Linux READ-ONLY raw USB benchmark. Same request generator,
// worker count, aligned buffers, timing and statistics; OS caches bypassed.
// No hashing in timed regions, no filesystem creation, and no write handles.
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Clock=std::chrono::steady_clock;
constexpr uint64_t MiB=1024*1024, GiB=1024*MiB;
constexpr uint64_t Base=GiB, Span=8*GiB, Seed=0x55425357415250ULL;
struct Config { std::string device,label,serial; uint64_t capacity; };
struct Case {
    const char *name; uint32_t block,depth; uint64_t bytes; double seconds; bool random;
    uint64_t base=Base,span=Span;
};
static std::string Trim(std::string s) {
    while(!s.empty() && (s.back()==' ' || s.back()=='\n' || s.back()=='\r' || s.back()=='\t')) s.pop_back();
    size_t first=s.find_first_not_of(" \t\r\n"); return first==std::string::npos ? "" : s.substr(first);
}
[[noreturn]] static void Failure(const char *op) {
#ifdef _WIN32
    auto e=GetLastError();
#else
    auto e=errno;
#endif
    throw std::runtime_error(std::string(op)+" error="+std::to_string(e));
}
static std::string Json(const std::string &s) {
    std::string out="\"";
    for(unsigned char c:s) {
        if(c=='\\' || c=='\"') out+='\\';
        if(c<32) throw std::runtime_error("control character in JSON field");
        out+=(char)c;
    }
    return out+'\"';
}
static double CpuSeconds() {
#ifdef _WIN32
    FILETIME created,exited,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) Failure("GetProcessTimes");
    return (double)(((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime)/1e7+
           (double)(((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime)/1e7;
#else
    rusage u{}; if(getrusage(RUSAGE_SELF,&u)) Failure("getrusage");
    return u.ru_utime.tv_sec+u.ru_utime.tv_usec/1e6+u.ru_stime.tv_sec+u.ru_stime.tv_usec/1e6;
#endif
}
static void VerifyIdentity(const Config &c) {
#ifdef _WIN32
    if(c.device.rfind("\\\\.\\PhysicalDrive",0)!=0) throw std::runtime_error("expected a physical disk path");
    HANDLE h=CreateFileA(c.device.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE) Failure("open metadata");
    try {
        GET_LENGTH_INFORMATION length{}; DWORD returned=0;
        if(!DeviceIoControl(h,IOCTL_DISK_GET_LENGTH_INFO,nullptr,0,&length,sizeof(length),&returned,nullptr)) Failure("get capacity");
        STORAGE_PROPERTY_QUERY query{}; query.PropertyId=StorageDeviceProperty; query.QueryType=PropertyStandardQuery;
        alignas(8) BYTE bytes[16384]{};
        if(!DeviceIoControl(h,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),bytes,sizeof(bytes),&returned,nullptr)) Failure("get USB identity");
        auto desc=(STORAGE_DEVICE_DESCRIPTOR *)bytes;
        if(returned<sizeof(*desc) || desc->BusType!=BusTypeUsb || !desc->SerialNumberOffset ||
           desc->SerialNumberOffset>=returned) throw std::runtime_error("not a USB disk with a serial number");
        const char *serial=(char *)bytes+desc->SerialNumberOffset;
        size_t n=strnlen_s(serial,returned-desc->SerialNumberOffset);
        if(n>=returned-desc->SerialNumberOffset || Trim(std::string(serial,n))!=c.serial ||
           (uint64_t)length.Length.QuadPart!=c.capacity) throw std::runtime_error("USB identity/capacity mismatch");
    } catch(...) { CloseHandle(h); throw; }
    CloseHandle(h);
#else
    struct stat st{}; if(stat(c.device.c_str(),&st) || !S_ISBLK(st.st_mode)) throw std::runtime_error("expected a block device");
    int fd=open(c.device.c_str(),O_RDONLY|O_CLOEXEC); if(fd<0) Failure("open metadata");
    uint64_t size=0; int error=ioctl(fd,BLKGETSIZE64,&size); close(fd);
    if(error) Failure("get capacity");
    if(size!=c.capacity) throw std::runtime_error("USB capacity mismatch");
    namespace fs=std::filesystem;
    auto name=fs::canonical(c.device).filename();
    fs::path block=fs::path("/sys/class/block")/name;
    if(fs::exists(block/"partition")) throw std::runtime_error("whole USB disk required");
    fs::path p=fs::canonical(block/"device"); bool found=false;
    for(;;) {
        if(fs::exists(p/"idVendor") && fs::exists(p/"idProduct") && fs::exists(p/"serial")) {
            std::ifstream f(p/"serial"); std::string serial; std::getline(f,serial);
            if(Trim(serial)!=c.serial) throw std::runtime_error("USB serial mismatch");
            found=true; break;
        }
        if(p==p.parent_path()) break;
        p=p.parent_path();
    }
    if(!found) throw std::runtime_error("target is not an identifiable USB disk");
#endif
    if(c.capacity<Base || Span>c.capacity-Base) throw std::runtime_error("benchmark range exceeds capacity");
}
struct Reader {
    void *buffer=nullptr; uint32_t size;
#ifdef _WIN32
    HANDLE file=INVALID_HANDLE_VALUE,event=nullptr;
#else
    int file=-1;
#endif
    Reader(const Config &c,uint32_t block):size(block) {
#ifdef _WIN32
        buffer=VirtualAlloc(nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!buffer) Failure("aligned allocation");
        file=CreateFileA(c.device.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                         OPEN_EXISTING,FILE_FLAG_NO_BUFFERING|FILE_FLAG_OVERLAPPED,nullptr);
        if(file==INVALID_HANDLE_VALUE) { VirtualFree(buffer,0,MEM_RELEASE); Failure("open direct disk"); }
        event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!event) { CloseHandle(file); VirtualFree(buffer,0,MEM_RELEASE); Failure("create read event"); }
#else
        if(posix_memalign(&buffer,4096,size)) throw std::runtime_error("aligned allocation failed");
        file=open(c.device.c_str(),O_RDONLY|O_CLOEXEC|O_DIRECT);
        if(file<0) { free(buffer); Failure("open direct disk"); }
#endif
        memset(buffer,0,size); // prefault before timing
    }
    Reader(const Reader &)=delete;
    ~Reader() {
#ifdef _WIN32
        CloseHandle(event); CloseHandle(file); VirtualFree(buffer,0,MEM_RELEASE);
#else
        close(file); free(buffer);
#endif
    }
    void Read(uint64_t offset) {
#ifdef _WIN32
        OVERLAPPED ov{}; ov.Offset=(DWORD)offset; ov.OffsetHigh=(DWORD)(offset>>32); ov.hEvent=event;
        BOOL ok=ReadFile(file,buffer,size,nullptr,&ov);
        if(!ok && GetLastError()!=ERROR_IO_PENDING) Failure("ReadFile");
        if(!ok && WaitForSingleObject(event,30000)!=WAIT_OBJECT_0) {
            CancelIoEx(file,&ov); DWORD done;
            GetOverlappedResult(file,&ov,&done,TRUE); // never free an outstanding buffer/OVERLAPPED
            throw std::runtime_error("disk read timeout");
        }
        DWORD done=0; if(!GetOverlappedResult(file,&ov,&done,FALSE)) Failure("read completion");
        if(done!=size) throw std::runtime_error("short disk read");
#else
        ssize_t done;
        do { done=pread(file,buffer,size,(off_t)offset); } while(done<0 && errno==EINTR);
        if(done<0) Failure("pread");
        if((uint32_t)done!=size) throw std::runtime_error("short disk read");
#endif
    }
};
static uint64_t Mix(uint64_t x) {
    x+=0x9e3779b97f4a7c15ULL;
    x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL;
    return x^(x>>31);
}
struct Worker {
    Reader reader;
    std::vector<double> latency;
    size_t count=0; std::string error;
    Clock::time_point finished;
    Worker(const Config &c,const Case &test):reader(c,test.block),latency(test.bytes ? (size_t)(test.bytes/test.block) : 1000000,0) {}
};
static void Run(const Config &c,const Case &test) {
    std::vector<std::unique_ptr<Worker>> workers;
    for(uint32_t i=0;i<test.depth;++i) workers.emplace_back(new Worker(c,test));
    // Matched direct warmup outside timing (512 requests for random cases).
    const uint64_t warmup=test.random ? 2*MiB : 64*MiB;
    for(uint64_t i=0;i<warmup/test.block;++i) workers[0]->reader.Read(test.base+(i*test.block)%test.span);
    std::mutex lock; std::condition_variable changed; unsigned ready=0; bool start=false;
    std::atomic<uint64_t> next{0}; std::atomic<bool> failed{false};
    Clock::time_point began,deadline;
    std::vector<std::thread> threads;
    for(auto &owner:workers) {
        Worker *w=owner.get();
        threads.emplace_back([&,w] {
            { std::unique_lock<std::mutex> guard(lock); ++ready; changed.notify_all(); changed.wait(guard,[&]{return start;}); }
            try {
                while(!failed.load(std::memory_order_relaxed)) {
                    if(!test.bytes && Clock::now()>=deadline) break;
                    uint64_t i=next.fetch_add(1,std::memory_order_relaxed);
                    if(test.bytes && i>=test.bytes/test.block) break;
                    uint64_t slot=(test.random ? Mix(i+Seed) : i)%(test.span/test.block);
                    auto before=Clock::now(); w->reader.Read(test.base+slot*test.block); auto after=Clock::now();
                    if(w->count>=w->latency.size()) throw std::runtime_error("latency sample capacity exceeded");
                    w->latency[w->count++]=std::chrono::duration<double,std::micro>(after-before).count();
                }
            } catch(const std::exception &e) { w->error=e.what(); failed.store(true); }
            w->finished=Clock::now();
        });
    }
    double cpuBefore;
    { std::unique_lock<std::mutex> guard(lock); changed.wait(guard,[&]{return ready==test.depth;});
      cpuBefore=CpuSeconds(); began=Clock::now();
      deadline=began+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(test.seconds));
      start=true; changed.notify_all(); }
    for(auto &thread:threads) thread.join();
    double cpu=CpuSeconds()-cpuBefore;
    std::vector<double> latency;
    auto ended=began;
    for(auto &w:workers) {
        if(!w->error.empty()) throw std::runtime_error(w->error);
        latency.insert(latency.end(),w->latency.begin(),w->latency.begin()+w->count);
        ended=std::max(ended,w->finished);
    }
    if(latency.empty()) throw std::runtime_error("no reads completed");
    uint64_t bytes=latency.size()*(uint64_t)test.block;
    if(test.bytes && bytes!=test.bytes) throw std::runtime_error("fixed read count mismatch");
    double elapsed=std::chrono::duration<double>(ended-began).count(),sum=0;
    for(auto us:latency) sum+=us;
    std::sort(latency.begin(),latency.end());
    auto percentile=[&](double p){ return latency[(size_t)((latency.size()-1)*p)]; };
#ifdef _WIN32
    const char *platform="windows";
#else
    const char *platform="linux";
#endif
    std::cout<<std::setprecision(10)<<"{\"platform\":"<<Json(platform)<<",\"label\":"<<Json(c.label)
        <<",\"case\":"<<Json(test.name)<<",\"direct\":true,\"read_only\":true,\"block_bytes\":"<<test.block
        <<",\"queue_depth\":"<<test.depth<<",\"offset_bytes\":"<<test.base<<",\"span_bytes\":"<<test.span
        <<",\"warmup_bytes\":"<<warmup<<",\"bytes\":"<<bytes<<",\"ops\":"<<latency.size()
        <<",\"seconds\":"<<elapsed<<",\"mib_s\":"<<bytes/elapsed/MiB<<",\"iops\":"<<latency.size()/elapsed
        <<",\"mean_us\":"<<sum/latency.size()<<",\"p50_us\":"<<percentile(.5)<<",\"p95_us\":"<<percentile(.95)
        <<",\"p99_us\":"<<percentile(.99)<<",\"max_us\":"<<latency.back()<<",\"process_cpu_s\":"<<cpu<<"}"<<std::endl;
}
static void Fingerprint(const Config &c) {
    Reader reader(c,4096); uint64_t hash=14695981039346656037ULL;
    for(uint64_t i=0;i<4096;++i) {
        reader.Read(Base+(Mix(i+Seed)%(Span/4096))*4096);
        for(size_t n=0;n<4096;++n) { hash^=((const unsigned char *)reader.buffer)[n]; hash*=1099511628211ULL; }
    }
    // Correctness check only. This operation has no timing/throughput output.
    std::cout<<"{\"label\":"<<Json(c.label)<<",\"case\":\"fingerprint\",\"algorithm\":\"fnv1a64\",\"samples\":4096,\"bytes\":16777216,\"hash\":\""
        <<std::hex<<std::setfill('0')<<std::setw(16)<<hash<<"\"}"<<std::endl;
}
int main(int argc,char **argv) {
    try {
        if(argc<5 || argc>6) throw std::runtime_error("usage: usb_read_bench DEVICE LABEL SERIAL CAPACITY_BYTES [CASE|all]");
        std::string cap=argv[4]; size_t end=0;
        if(cap.empty() || cap.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("invalid capacity");
        Config c{argv[1],argv[2],argv[3],std::stoull(cap,&end)};
        VerifyIdentity(c);
        if(argc==6 && !strcmp(argv[5],"fingerprint")) { Fingerprint(c); return 0; }
        const Case tests[]={
            {"seq_1m_q1",(uint32_t)MiB,1,GiB,0,false},
            {"seq_1m_q4",(uint32_t)MiB,4,GiB,0,false},
            {"seq_64k_q1",65536,1,512*MiB,0,false},
            {"rand_4k_q1",4096,1,0,10,true},
            {"rand_4k_q4",4096,4,0,10,true},
            {"prefix64m_1m_q1",(uint32_t)MiB,1,GiB,0,false,0,64*MiB},
            {"prefix64m_1m_q4",(uint32_t)MiB,4,GiB,0,false,0,64*MiB}
        };
        bool ran=false;
        for(auto &test:tests) if(argc==5 || !strcmp(argv[5],"all") || !strcmp(argv[5],test.name)) { Run(c,test); ran=true; }
        if(!ran) throw std::runtime_error("unknown case");
        return 0;
    } catch(const std::exception &e) { std::cerr<<"Read-only benchmark failed: "<<e.what()<<std::endl; return 1; }
}
