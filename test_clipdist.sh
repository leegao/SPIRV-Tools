time sh build_host.sh
host_build/tools/spirv-opt --remove-clip-cull-dist clip_dist.spv -o clip_dist_opt.spv
spirv-dis clip_dist.spv > clip_dist.spv.txt
spirv-dis clip_dist_opt.spv > clip_dist_opt.spv.txt
spirv-val clip_dist_opt.spv
