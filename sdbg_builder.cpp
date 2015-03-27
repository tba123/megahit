#include <stdio.h>
#include <omp.h>

#include <iostream>
#include <string>
#include <stdexcept>

#include "cx1_kmer_count.h"
#include "cx1_edge2sdbg.h"
#include "lv2_gpu_functions.h"
#include "options_description.h"
#include "utils.h"

AutoMaxRssRecorder recorder; 

int main_kmer_count(int argc, char **argv) {
	// parse option
	OptionsDescription desc;
	count_opt_t opt;

    desc.AddOption("kmer_k", "k", opt.kmer_k, "kmer size");
    desc.AddOption("min_kmer_frequency", "m", opt.kmer_freq_threshold, "min frequency to output an edge");
    desc.AddOption("host_mem", "", opt.host_mem, "Max memory to be used. 90% of the free memory is recommended.");
    desc.AddOption("gpu_mem", "", opt.gpu_mem, "gpu memory to be used. 0 for auto detect.");
    desc.AddOption("max_read_length", "", opt.max_read_length, "max read length");
    desc.AddOption("num_cpu_threads", "", opt.num_cpu_threads, "number of CPU threads. At least 2.");
    desc.AddOption("num_output_threads", "", opt.num_output_threads, "number of threads for output. Must be less than num_cpu_threads");
    desc.AddOption("input_file", "", opt.input_file, "input fast[aq] file, can be gzip'ed. \"-\" for stdin.");
    desc.AddOption("output_prefix", "", opt.output_prefix, "output prefix");
    desc.AddOption("mem_flag", "", opt.mem_flag, "memory options. 0: minimize memory usage; 1: automatically use moderate memory; other: use all available mem specified by '--host_mem'");

    try {
        desc.Parse(argc, argv);
        if (opt.input_file == "") {
            throw std::logic_error("No input file!");
        }
        if (opt.num_cpu_threads == 0) {
            opt.num_cpu_threads = omp_get_max_threads();
        }
        if (opt.num_output_threads == 0) {
            opt.num_output_threads = std::max(1, opt.num_cpu_threads / 3);
        }
        if (opt.host_mem == 0) {
            throw std::logic_error("Please specify the host memory!");
        }
        if (opt.gpu_mem == 0) {
#ifndef DISABLE_GPU
            size_t free_gpu_mem, total_gpu_mem;
            get_cuda_memory(free_gpu_mem, total_gpu_mem);
            opt.gpu_mem = free_gpu_mem;
#else 
            opt.gpu_mem = 0;
#endif
        }

        if (opt.num_cpu_threads == 1) {
            throw std::logic_error("Number of CPU threads should be at least 2!");
        }
        if (opt.num_output_threads >= opt.num_cpu_threads) {
            throw std::logic_error("Number of output threads must be less than number of CPU threads!");
        }
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << "Usage: sdbg_builder count --input_file fastx_file -o out" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << desc << std::endl;
        exit(1);
    }

    cx1_kmer_count::count_global_t globals;
    globals.kmer_k = opt.kmer_k;
    globals.kmer_freq_threshold = opt.kmer_freq_threshold;
    globals.max_read_length = opt.max_read_length;
    globals.host_mem = opt.host_mem;
    globals.gpu_mem = opt.gpu_mem;
    globals.num_cpu_threads = opt.num_cpu_threads;
    globals.num_output_threads = opt.num_output_threads;
    globals.input_file = opt.input_file.c_str();
    globals.output_prefix = opt.output_prefix.c_str();
    globals.mem_flag = opt.mem_flag;

    log ("[C::%s] Host memory to be used: %lld\n", __func__, (long long)globals.host_mem);
    log ("[C::%s] Number CPU threads: %d\n", __func__, globals.num_cpu_threads);

    // set & run cx1
    globals.cx1.g_ = &globals;
    globals.cx1.encode_lv1_diff_base_func_ = cx1_kmer_count::encode_lv1_diff_base;
	globals.cx1.prepare_func_ = cx1_kmer_count::read_input_prepare;
	globals.cx1.lv0_calc_bucket_size_func_ = cx1_kmer_count::lv0_calc_bucket_size;
	globals.cx1.init_global_and_set_cx1_func_ = cx1_kmer_count::init_global_and_set_cx1;
	globals.cx1.lv1_fill_offset_func_ = cx1_kmer_count::lv1_fill_offset;
	globals.cx1.lv2_extract_substr_func_ = cx1_kmer_count::lv2_extract_substr;
	globals.cx1.lv2_sort_func_ = cx1_kmer_count::lv2_sort;
	globals.cx1.lv2_pre_output_partition_func_ = cx1_kmer_count::lv2_pre_output_partition;
	globals.cx1.lv2_output_func_ = cx1_kmer_count::lv2_output;
	globals.cx1.lv2_post_output_func_ = cx1_kmer_count::lv2_post_output;
	globals.cx1.post_proc_func_ = cx1_kmer_count::post_proc;

	globals.cx1.run();

    return 0;
}

int main_edge2sdbg(int argc, char **argv) {
    OptionsDescription desc;
    edge2sdbg_opt_t opt;

    desc.AddOption("host_mem", "", opt.host_mem, "memory to be used. No more than 95% of the free memory is recommended. 0 for auto detect.");
    desc.AddOption("gpu_mem", "", opt.gpu_mem, "gpu memory to be used. 0 for auto detect.");
    desc.AddOption("num_cpu_threads", "t", opt.num_cpu_threads, "number of CPU threads. At least 2.");
    desc.AddOption("num_output_threads", "", opt.num_output_threads, "number of threads for output. Must be less than num_cpu_threads");
    desc.AddOption("input_prefix", "", opt.input_prefix, "files input_prefix.edges.* output by count module, can be gzip'ed.");
    desc.AddOption("num_edge_files", "", opt.num_edge_files, "the number of files with name input_prefix.edges.*");
    desc.AddOption("output_prefix", "o", opt.output_prefix, "output prefix");
    desc.AddOption("need_mercy", "", opt.need_mercy, "to add mercy edges. The file input_prefix.cand output by count module should exist.");
    desc.AddOption("max_read_length", "", opt.max_read_length, "max read length");
    desc.AddOption("mem_flag", "", opt.mem_flag, "memory options. 0: minimize memory usage; 1: automatically use moderate memory; other: use all available mem specified by '--host_mem'");

    try {
        desc.Parse(argc, argv);
        if (opt.input_prefix == "") {
            throw std::logic_error("No input prefix!");
        }
        if (opt.num_edge_files == 0) {
            throw std::logic_error("Number of edge files cannot be 0!");
        }

        if (opt.num_cpu_threads == 0) {
            opt.num_cpu_threads = omp_get_max_threads();
        }

        if (opt.num_output_threads == 0) {
            opt.num_output_threads = std::max(1, opt.num_cpu_threads / 3);
        }

        if (opt.host_mem == 0) {
            throw std::logic_error("Please specify the host memory!");
        }

        if (opt.gpu_mem == 0) {
#ifndef DISABLE_GPU
            size_t free_gpu_mem, total_gpu_mem;
            get_cuda_memory(free_gpu_mem, total_gpu_mem);
            opt.gpu_mem = free_gpu_mem;
#else
            opt.gpu_mem = 0;
#endif
        }

        if (opt.num_cpu_threads == 1) {
            throw std::logic_error("Number of CPU threads is at least 2!");
        }
        if (opt.num_output_threads >= opt.num_cpu_threads) {
            throw std::logic_error("Number of output threads must be less than number of CPU threads!");
        }
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << "Usage: sdbg_builder build --input_prefix input --num_edge_files num -o out" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << desc << std::endl;
        exit(1);
    }

    cx1_edge2sdbg::edge2sdbg_global_t globals;
    globals.need_mercy = opt.need_mercy;
    globals.host_mem = opt.host_mem;
    globals.gpu_mem = opt.gpu_mem;
    globals.num_cpu_threads = opt.num_cpu_threads;
    globals.num_edge_files = opt.num_edge_files;
    globals.num_output_threads = opt.num_output_threads;
    globals.input_prefix = opt.input_prefix.c_str();
    globals.output_prefix = opt.output_prefix.c_str();
    globals.max_read_length = opt.max_read_length;
    globals.mem_flag = opt.mem_flag;

    log("[B::%s] Host memory to be used: %lld\n", __func__, (long long)globals.host_mem);
    log("[B::%s] Number CPU threads: %d\n", __func__, globals.num_cpu_threads);

    // set & run cx1
    globals.cx1.g_ = &globals;
    globals.cx1.encode_lv1_diff_base_func_ = cx1_edge2sdbg::encode_lv1_diff_base;
    globals.cx1.prepare_func_ = cx1_edge2sdbg::read_edges_and_prepare;
    globals.cx1.lv0_calc_bucket_size_func_ = cx1_edge2sdbg::lv0_calc_bucket_size;
    globals.cx1.init_global_and_set_cx1_func_ = cx1_edge2sdbg::init_global_and_set_cx1;
    globals.cx1.lv1_fill_offset_func_ = cx1_edge2sdbg::lv1_fill_offset;
    globals.cx1.lv2_extract_substr_func_ = cx1_edge2sdbg::lv2_extract_substr;
    globals.cx1.lv2_sort_func_ = cx1_edge2sdbg::lv2_sort;
    globals.cx1.lv2_pre_output_partition_func_ = cx1_edge2sdbg::lv2_pre_output_partition;
    globals.cx1.lv2_output_func_ = cx1_edge2sdbg::lv2_output;
    globals.cx1.lv2_post_output_func_ = cx1_edge2sdbg::lv2_post_output;
    globals.cx1.post_proc_func_ = cx1_edge2sdbg::post_proc;

    globals.cx1.run();
    return 0;
}

void DisplayHelp(char *program_name) {
    fprintf(stderr, "Usage: \n");
    fprintf(stderr, "    1. Counting & output solid edges: \n");
    fprintf(stderr, "       type \"%s count\" for help.\n", program_name);
    fprintf(stderr, "    2. Build Succinct dBG from solid edges: \n");
    fprintf(stderr, "       type \"%s build\" for help.\n", program_name);
    fprintf(stderr, "    3. 1-pass count & build Succinct dBG: \n");
    fprintf(stderr, "       type \"%s 1pass\" for help.\n", program_name);
}

int main(int argc, char** argv) {
    if (argc < 2 || (std::string(argv[1]) != "count" && std::string(argv[1]) != "build" && std::string(argv[1]) != "1pass")) {
        DisplayHelp(argv[0]);
        exit(1);
    }

    if (std::string(argv[1]) == "count") {
    	return main_kmer_count(argc - 1, argv + 1);
    } else if (std::string(argv[1]) == "build") {
        return main_edge2sdbg(argc - 1, argv + 1);
    }

    return 0;
}