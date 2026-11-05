// Compiles mosh_kernels.cl as CUDA with NVRTC and writes PTX.
// usage: gen_ptx <kernels.cl> <out.ptx> [compute_XX]; run through tools/gen_ptx.sh
#include <nvrtc.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s kernels.cl out.ptx [compute_XX]\n", argv[0]); return 2; }
    std::ifstream in(argv[1]);
    std::stringstream ss; ss << in.rdbuf();
    const std::string src = ss.str();
    nvrtcProgram prog;
    if (nvrtcCreateProgram(&prog, src.c_str(), "mosh_kernels.cu", 0, nullptr, nullptr) != NVRTC_SUCCESS) return 1;
    const std::string arch = std::string("--gpu-architecture=") + (argc > 3 ? argv[3] : "compute_50");
    const char* opts[] = {arch.c_str(), "--fmad=false", "--prec-div=true", "--prec-sqrt=true", "--ftz=false", "-default-device"};
    nvrtcResult r = nvrtcCompileProgram(prog, 6, opts);
    size_t logSize = 0; nvrtcGetProgramLogSize(prog, &logSize);
    std::vector<char> log(logSize + 1); nvrtcGetProgramLog(prog, log.data());
    if (logSize > 1) fprintf(stderr, "%s\n", log.data());
    if (r != NVRTC_SUCCESS) { fprintf(stderr, "compile failed: %s\n", nvrtcGetErrorString(r)); return 1; }
    size_t n = 0; nvrtcGetPTXSize(prog, &n);
    std::vector<char> ptx(n); nvrtcGetPTX(prog, ptx.data());
    FILE* fo = fopen(argv[2], "wb"); fwrite(ptx.data(), 1, n ? n - 1 : 0, fo); fclose(fo);
    printf("PTX %zu bytes\n", n);
    return 0;
}
