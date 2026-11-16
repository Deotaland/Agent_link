#include "boot_animation_stubs.h"
#include <fstream>
#define fopen boot_fixture::Open
#include "../factory/sdgoods-esp32s3/boot_animation.cc"
#undef fopen
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
    boot_fixture::bytes.assign(std::istreambuf_iterator<char>(input),{});
    const auto original=boot_fixture::bytes;
    sugarat_boot::Movie movie;
    assert(movie.Open(original.data(),original.size()) && movie.frames>0 && movie.pcm_bytes>0);
    assert(movie.FrameAt(0)==0 && movie.FrameAt(movie.DurationUs()+100000)==movie.frames-1);
    {
        AmoledHardware hardware;PlaySugaratBootAnimation(hardware);boot_fixture::Join();
        assert(boot_fixture::frames>0 && boot_fixture::frames<=int(movie.frames));
        assert(hardware.audio_.samples==movie.pcm_bytes/2 && hardware.panel_.brightness==190);
        assert(boot_fixture::mounts==1 && boot_fixture::unmounts==1 && !boot_fixture::allocations);
    }
    boot_fixture::frames=0;boot_fixture::bytes.back()^=1;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(!boot_fixture::frames);}
    boot_fixture::bytes=original;boot_fixture::bytes.resize(24);
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(!boot_fixture::frames);}
    boot_fixture::bytes=original;boot_fixture::file_missing=true;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(!boot_fixture::frames);}
    boot_fixture::file_missing=false;boot_fixture::mount_fails=true;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(!boot_fixture::frames);}
    boot_fixture::mount_fails=false;boot_fixture::decode_fails=true;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(!boot_fixture::frames);}
    boot_fixture::decode_fails=false;boot_fixture::task_fails=true;boot_fixture::decode_us=80000;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(boot_fixture::frames>0 && boot_fixture::frames<int(movie.frames));}
    boot_fixture::frames=0;
    auto before_wake=boot_fixture::mounts;
    boot_fixture::sleep_resume=true;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);
     assert(!boot_fixture::frames && boot_fixture::mounts==before_wake && !hardware.audio_.samples);}
    // The marker is consumed: a second invocation cannot skip another startup.
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(boot_fixture::frames>0);}
    boot_fixture::frames=0;
    // A new power start and USB off-state wake both play the normal movie.
    boot_fixture::sleep_resume=false;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(boot_fixture::frames>0);}
    const auto mounts=boot_fixture::mounts;boot_fixture::partition_address=0;
    {AmoledHardware hardware;PlaySugaratBootAnimation(hardware);assert(boot_fixture::mounts==mounts);}
    assert(!boot_fixture::allocations && boot_fixture::workers.empty());
    puts("Boot animation: video/audio, frame timing, CRC/errors, one-shot sleep resume, cold/off playback, geometry and cleanup passed.");
}
