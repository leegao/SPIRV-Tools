time sh build_host.sh
host_build/tools/spirv-opt --mali-optimization-barrier optbarrier.spv -o optbarrier_opt.spv
spirv-dis optbarrier.spv > optbarrier.spv.txt
spirv-dis optbarrier_opt.spv > optbarrier_opt.spv.txt
