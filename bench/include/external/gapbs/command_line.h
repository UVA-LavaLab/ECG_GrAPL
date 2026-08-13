// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef COMMAND_LINE_H_
#define COMMAND_LINE_H_

#include <getopt.h>

#include "util.h"
#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

/*
   GAP Benchmark Suite
   Class:  CLBase
   Author: Scott Beamer

   Handles command line argument parsing
   - Through inheritance, can add more options to object
   - For example, most kernels will use CLApp
 */

class CLBase
{
protected:
    int argc_;
    char **argv_;
    std::string name_;
    std::string get_args_ = "f:g:hk:su:m:o:zj:SlD:";
    std::vector<std::string> help_strings_;
    std::vector<std::pair<ReorderingAlgo, std::vector<std::string> > >
    reorder_options_;

    int scale_ = -1;
    int degree_ = 16;
    std::string filename_ = "";
    bool symmetrize_ = false;
    bool uniform_ = false;
    bool keep_self_ = false;
    bool in_place_ = false;
    bool use_out_degree_ = true;
    std::vector<int> segments_{0, 1, 1};   // Default to one segment ir
    bool enable_logging_ = false;
    std::string db_dir_ = "";              // --db-dir: JSON database output directory

    void AddHelpLine(char opt, std::string opt_arg, std::string text,
                     std::string def = "")
    {
        const int kBufLen = 200;
        char buf[kBufLen];
        if (opt_arg != "")
            opt_arg = "<" + opt_arg + ">";
        if (def != "")
            def = "[" + def + "]";
        snprintf(buf, kBufLen, " -%c %-9s: %-54s%10s", opt, opt_arg.c_str(),
                 text.c_str(), def.c_str());
        help_strings_.push_back(buf);
    }

public:
    CLBase(int argc, char **argv, std::string name = "")
        : argc_(argc), argv_(argv), name_(name)
    {
        AddHelpLine('h', "", "print this help message");
        AddHelpLine('f', "file", "load graph from file");
        AddHelpLine('s', "", "symmetrize input edge list", "false");
        AddHelpLine('g', "scale", "generate 2^scale kronecker graph");
        AddHelpLine('u', "scale", "generate 2^scale uniform-random graph");
        AddHelpLine('k', "degree", "average degree for synthetic graph",
                    std::to_string(degree_));
        AddHelpLine('m', "", "reduces memory usage during graph building", "false");
        AddHelpLine('o', "order",
                    "apply reordering strategy, optionally with a parameter \n     "
                    "          [example]-o <12|13>:option1:option2 "
                    "-o 2 -o 14:mapping.label",
                    "optional");
        AddHelpLine('z', "indegree",
                    "use indegree for ordering [Degree Based Orderings]", "false");
        AddHelpLine('j', "segments", "number of segments for the graph \n     "
                    "          [type:n:m] <0:GRAPHIT/Cagra> <1:TRUST>", "0:1:1");
        AddHelpLine('l', "", "log performance within each trial", "false");
        AddHelpLine('S', "", "keep self loops", "false");
        AddHelpLine('D', "dir", "database directory for JSON output", "results/data");
    }

    bool ParseArgs()
    {
        signed char c_opt;
        extern char *optarg; // from and for getopt
        while ((c_opt = getopt(argc_, argv_, get_args_.c_str())) != -1)
        {
            HandleArg(c_opt, optarg);
        }
        if ((filename_ == "") && (scale_ == -1))
        {
            std::cout << "No graph input specified. (Use -h for help)" << std::endl;
            return false;
        }
        if (scale_ != -1)
            symmetrize_ = true;
        return true;
    }

    void virtual HandleArg(signed char opt, char *opt_arg)
    {
        switch (opt)
        {
        case 'f':
            filename_ = std::string(opt_arg);
            break;
        case 'g':
            scale_ = atoi(opt_arg);
            break;
        case 'h':
            PrintUsage();
            break;
        case 'k':
            degree_ = atoi(opt_arg);
            break;
        case 'z':
            use_out_degree_ = false;
            break;
        case 's':
            symmetrize_ = true;
            break;
        case 'u':
            uniform_ = true;
            scale_ = atoi(opt_arg);
            break;
        case 'm':
            in_place_ = true;
            break;
        case 'l':
            enable_logging_ = true;
            break;
        case 'S':
            keep_self_ = true;
            break;
        case 'D':
            db_dir_ = std::string(opt_arg);
            break;
        case 'o':
        {
            std::string arg(opt_arg);
            size_t pos = arg.find(':');
            ReorderingAlgo algo =
                static_cast<ReorderingAlgo>(std::stoi(arg.substr(0, pos)));
            std::vector<std::string> params;
            if (pos != std::string::npos)
            {
                size_t start = pos + 1;
                size_t end;
                while ((end = arg.find(':', start)) != std::string::npos)
                {
                    params.push_back(arg.substr(start, end - start));
                    start = end + 1;
                }
                params.push_back(arg.substr(start));
            }
            reorder_options_.emplace_back(algo, params);
        }
        break;
        case 'j':
        {
            std::string arg(opt_arg);
            size_t start = 0;
            size_t end = arg.find(':');
            size_t index = 0;

            while (end != std::string::npos)
            {
                if (index < segments_.size())
                {
                    segments_[index] = std::stoi(arg.substr(start, end - start));
                }
                else
                {
                    segments_.push_back(std::stoi(arg.substr(start, end - start)));
                }
                start = end + 1;
                end = arg.find(':', start);
                ++index;
            }

            // Handle the last segment
            if (index < segments_.size())
            {
                segments_[index] = std::stoi(arg.substr(start));
            }
            else
            {
                segments_.push_back(std::stoi(arg.substr(start)));
            }
        }
        break;
        }
    }

    void PrintUsage()
    {
        std::cout << name_ << std::endl;
        // std::sort(help_strings_.begin(), help_strings_.end());
        for (std::string h : help_strings_)
            std::cout << h << std::endl;
        std::exit(0);
    }

    int scale() const
    {
        return scale_;
    }
    int degree() const
    {
        return degree_;
    }
    std::string filename() const
    {
        return filename_;
    }
    bool symmetrize() const
    {
        return symmetrize_;
    }
    bool keep_self() const
    {
        return keep_self_;
    }
    void set_keep_self(bool keep_self_loop = false)
    {
        keep_self_ = keep_self_loop;
    }
    bool uniform() const
    {
        return uniform_;
    }
    bool in_place() const
    {
        return in_place_;
    }
    bool use_out_degree() const
    {
        return use_out_degree_;
    }
    bool logging_en() const
    {
        return enable_logging_;
    }
    const std::vector<std::pair<ReorderingAlgo, std::vector<std::string> > > &
    reorder_options() const
    {
        return reorder_options_;
    }
    const std::vector<int> &segments() const
    {
        return segments_;
    }
    std::string db_dir() const
    {
        return db_dir_;
    }
};

class CLApp : public CLBase
{
    bool do_analysis_ = false;
    int num_trials_ = 16;
    int64_t start_vertex_ = -1;
    bool do_verify_ = false;

public:
    CLApp(int argc, char **argv, std::string name) : CLBase(argc, argv, name)
    {
        get_args_ += "an:r:v";
        AddHelpLine('a', "", "output analysis of last run", "false");
        AddHelpLine('n', "n", "perform n trials", std::to_string(num_trials_));
        AddHelpLine('r', "node", "start from node r", "rand");
        AddHelpLine('v', "", "verify the output of each run", "false");
    }

    void HandleArg(signed char opt, char *opt_arg) override
    {
        switch (opt)
        {
        case 'a':
            do_analysis_ = true;
            break;
        case 'n':
            num_trials_ = atoi(opt_arg);
            break;
        case 'r':
            start_vertex_ = atol(opt_arg);
            break;
        case 'v':
            do_verify_ = true;
            break;
        default:
            CLBase::HandleArg(opt, opt_arg);
        }
    }
    bool do_analysis() const
    {
        return do_analysis_;
    }
    int num_trials() const
    {
        return num_trials_;
    }
    int64_t start_vertex() const
    {
        return start_vertex_;
    }
    bool do_verify() const
    {
        return do_verify_;
    }
};

class CLIterApp : public CLApp
{
    int num_iters_;

public:
    CLIterApp(int argc, char **argv, std::string name, int num_iters)
        : CLApp(argc, argv, name), num_iters_(num_iters)
    {
        get_args_ += "i:";
        AddHelpLine('i', "i", "perform i iterations", std::to_string(num_iters_));
    }

    void HandleArg(signed char opt, char *opt_arg) override
    {
        switch (opt)
        {
        case 'i':
            num_iters_ = atoi(opt_arg);
            break;
        default:
            CLApp::HandleArg(opt, opt_arg);
        }
    }

    int num_iters() const
    {
        return num_iters_;
    }
};

class CLPageRank : public CLApp
{
    int max_iters_;
    double tolerance_;

public:
    CLPageRank(int argc, char **argv, std::string name, double tolerance,
               int max_iters)
        : CLApp(argc, argv, name), max_iters_(max_iters), tolerance_(tolerance)
    {
        get_args_ += "i:t:";
        AddHelpLine('i', "i", "perform at most i iterations",
                    std::to_string(max_iters_));
        AddHelpLine('t', "t", "use tolerance t", std::to_string(tolerance_));
    }

    void HandleArg(signed char opt, char *opt_arg) override
    {
        switch (opt)
        {
        case 'i':
            max_iters_ = atoi(opt_arg);
            break;
        case 't':
            tolerance_ = std::stod(opt_arg);
            break;
        default:
            CLApp::HandleArg(opt, opt_arg);
        }
    }

    int max_iters() const
    {
        return max_iters_;
    }
    double tolerance() const
    {
        return tolerance_;
    }
};

template <typename WeightT_> class CLDelta : public CLApp
{
    WeightT_ delta_ = 1;

public:
    CLDelta(int argc, char **argv, std::string name) : CLApp(argc, argv, name)
    {
        get_args_ += "d:";
        AddHelpLine('d', "d", "delta parameter", std::to_string(delta_));
    }

    void HandleArg(signed char opt, char *opt_arg) override
    {
        switch (opt)
        {
        case 'd':
            if (std::is_floating_point<WeightT_>::value)
                delta_ = static_cast<WeightT_>(atof(opt_arg));
            else
                delta_ = static_cast<WeightT_>(atol(opt_arg));
            break;
        default:
            CLApp::HandleArg(opt, opt_arg);
        }
    }

    WeightT_ delta() const
    {
        return delta_;
    }
};

class CLConvert : public CLBase
{
    std::string out_filename_ = "";
    std::string label_out_filename_ = "";
    bool out_weighted_ = false;
    bool out_el_ = false;
    bool out_mtx_ = false;
    bool out_sg_ = false;
    bool out_ligra_ = false;
    bool out_structures_ = false;
    bool out_label_so_ = false;
    bool out_label_lo_ = false;

public:
    CLConvert(int argc, char **argv, std::string name)
        : CLBase(argc, argv, name)
    {
        get_args_ += "e:b:x:q:p:y:V:w";
        AddHelpLine('b', "file", "output serialized graph to file (.sg)");
        AddHelpLine('e', "file", "output edge list to file (.el)");
        AddHelpLine('V', "file", "output separate files each has csr array content .out_degree/.out_neigh/.offset");
        AddHelpLine('p', "file",
                    "output matrix market exchange format to file (.mtx)");
        AddHelpLine('y', "file",
                    "output in Ligra adjacency graph format to file (.ligra)");
        AddHelpLine('w', "file", "make output weighted (.wel|.wsg)");
        AddHelpLine('x', "file", "output new reordered labels to file list (.so)");
        AddHelpLine('q', "file",
                    "output new reordered labels to file serialized (.lo)");
    }

    void HandleArg(signed char opt, char *opt_arg) override
    {
        switch (opt)
        {
        case 'b':
            out_sg_ = true;
            out_filename_ = std::string(opt_arg);
            break;
        case 'p':
            out_mtx_ = true;
            out_filename_ = std::string(opt_arg);
            break;
        case 'y':
            out_ligra_ = true;
            out_filename_ = std::string(opt_arg);
            break;
        case 'V':
            out_structures_ = true;
            out_filename_ = std::string(opt_arg);
            break;
        case 'x':
            out_label_so_ = true;
            label_out_filename_ = std::string(opt_arg);
            break;
        case 'q':
            out_label_lo_ = true;
            label_out_filename_ = std::string(opt_arg);
            break;
        case 'e':
            out_el_ = true;
            out_filename_ = std::string(opt_arg);
            break;
        case 'w':
            out_weighted_ = true;
            break;
        default:
            CLBase::HandleArg(opt, opt_arg);
        }
    }

    std::string out_filename() const
    {
        return out_filename_;
    }
    std::string label_out_filename() const
    {
        return label_out_filename_;
    }
    bool out_weighted() const
    {
        return out_weighted_;
    }
    bool out_structures() const
    {
        return out_structures_;
    }
    bool out_el() const
    {
        return out_el_;
    }
    bool out_label_so() const
    {
        return out_label_so_;
    }
    bool out_label_lo() const
    {
        return out_label_lo_;
    }
    bool out_sg() const
    {
        return out_sg_;
    }
    bool out_mtx() const
    {
        return out_mtx_;
    }
    bool out_ligra() const
    {
        return out_ligra_;
    }
};

#endif // COMMAND_LINE_H_
