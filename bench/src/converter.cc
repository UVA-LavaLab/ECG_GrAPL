// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#include <iostream>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "writer.h"

using namespace std;

int main(int argc, char *argv[])
{
    CLConvert cli(argc, argv, "converter");
    cli.ParseArgs();
    graphbrew::database::InitSelfRecording(cli.db_dir());
    if (cli.out_weighted())
    {
        WeightedBuilder bw(cli);
        WGraph wg = bw.MakeGraph();
        wg.PrintStats();
        WeightedWriter ww(wg);
        if (cli.out_sg() || cli.out_el() || cli.out_mtx() || cli.out_ligra() || cli.out_structures())
            ww.WriteGraph(cli.out_filename(), cli.out_sg(), cli.out_mtx(), cli.out_el(), cli.out_ligra(), cli.out_structures());
        if (cli.out_label_so() || cli.out_label_lo())
        {
            ww.WriteLabels(cli.label_out_filename(), cli.out_label_so(), cli.out_label_lo());
        }

    }
    else
    {
        Builder b(cli);
        Graph g = b.MakeGraph();
        g.PrintStats();
        Writer w(g);
        if (cli.out_sg() || cli.out_el() || cli.out_mtx() || cli.out_ligra() || cli.out_structures())
            w.WriteGraph(cli.out_filename(), cli.out_sg(), cli.out_mtx(), cli.out_el(), cli.out_ligra(), cli.out_structures());
        if (cli.out_label_so() || cli.out_label_lo())
        {
            w.WriteLabels(cli.label_out_filename(), cli.out_label_so(), cli.out_label_lo());
        }
    }

    return 0;
}
