//  ********************************************************************
//  This file is part of KAT - the K-mer Analysis Toolkit.
//
//  KAT is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  KAT is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with KAT.  If not, see <http://www.gnu.org/licenses/>.
//  *******************************************************************

#include <config.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <thread>
using std::shared_ptr;
using std::make_shared;
using std::thread;

#include <boost/exception/all.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;
namespace bfs = boost::filesystem;
using bfs::path;

#include <matrix/matrix_metadata_extractor.hpp>

#include <jellyfish/mer_dna.hpp>
#include <jellyfish_helper.hpp>

#include "histogram.hpp"

kat::Histogram::Histogram(vector<path> _inputs, uint64_t _low, uint64_t _high, uint64_t _inc)
     : inputs(_inputs), low(_low), high(_high), inc(_inc) {
         
    // Calculate other vars required for this run
    base = calcBase();
    ceil = calcCeil();
    nb_buckets = ceil + 1 - base;     
}


void kat::Histogram::execute() {

    // Some validation first
    if (high < low) {
        BOOST_THROW_EXCEPTION(HistogramException() << HistogramErrorInfo(string(
                "High count value must be >= to low count value.  High: ") + lexical_cast<string>(high) + 
                "; Low: " + lexical_cast<string>(low))); 
    }

    // Create jellyfish hash if required
    hashFile = path(outputPrefix.string() + string(".jf") + lexical_cast<string>(merLen));
    path hashOutput = JellyfishHelper::jellyfishCount(inputs, hashFile, merLen, hashSize, threads, canonical, true);
    if (hashOutput != hashFile) {
        bfs::create_symlink(hashOutput, hashFile);
    }            

    // Setup handles to load hashes
    jfh = make_shared<JellyfishHelper>(hashFile, AccessMethod::SEQUENTIAL);

    loadHashes();

    
    
    data = vector<uint64_t>(nb_buckets, 0);
    threadedData = vector<shared_ptr<vector<uint64_t>>>();
    
    
    std::ostream* out_stream = verbose ? &cerr : (std::ostream*)0;

    // Do the work
    startAndJoinThreads();
    
    merge();
}

void kat::Histogram::print(std::ostream &out) {
    // Output header
    out << mme::KEY_TITLE << "K-mer spectra for: " << hashFile << endl;
    out << mme::KEY_X_LABEL << "K" << jfh->getKeyLen() << " multiplicity: " << hashFile << endl;
    out << mme::KEY_Y_LABEL << "Number of distinct K" << jfh->getKeyLen() << " mers" << endl;
    out << mme::MX_META_END << endl;

    uint64_t col = base;
    for (uint64_t i = 0; i < nb_buckets; i++, col += inc) {
        out << col << " " << data[i] << "\n";
    }
}


void kat::Histogram::loadHashes() {

    auto_cpu_timer timer(1, "  Time taken: %ws\n\n");        

    cout << "Loading hash into memory...";
    cout.flush();

    jfh->load();

    cout << " done.";
    cout.flush();
}

void kat::Histogram::merge() {
    auto_cpu_timer timer(1, "  Time taken: %ws\n\n");        

    cout << "Merging counts from each thread...";
    cout.flush();

    for(size_t i = 0; i < nb_buckets; i++) {
        for(size_t j = 0; j < threads; j++) {
            data[i] += threadedData[j]->at(i);
        }
    }
    
    cout << " done.";
    cout.flush();
}

void kat::Histogram::startAndJoinThreads() {

    auto_cpu_timer timer(1, "  Time taken: %ws\n\n");        

    cout << "Counting kmers in hash with " << threads << " threads ...";
    cout.flush();

    thread t[threads];

    for(int i = 0; i < threads; i++) {
        t[i] = thread(&Histogram::start, this, i);
    }

    for(int i = 0; i < threads; i++){
        t[i].join();
    }

    cout << "done.";
    cout.flush();
}

void kat::Histogram::start(int th_id) {
    
    shared_ptr<vector<uint64_t>> hist = make_shared<vector<uint64_t>>(nb_buckets);
    
    lha::region_iterator it = jfh->getRegionSlice(th_id, threads);
    while (it.next()) {
        uint64_t val = it.val();
        if (val < base)
            ++(*hist)[0];
        else if (val > ceil)
            ++(*hist)[nb_buckets - 1];
        else
            ++(*hist)[(val - base) / inc];
    }
    
    threadedData.push_back(hist);
}

int kat::Histogram::main(int argc, char *argv[]) {

    vector<path>    inputs;
    path            output_prefix;
    uint16_t        threads;
    uint64_t        low;
    uint64_t        high;
    uint64_t        inc;
    bool            canonical;
    uint16_t        mer_len;
    uint64_t        hash_size;            
    bool            verbose;
    bool            help;

    // Declare the supported options.
    po::options_description generic_options(Histogram::helpMessage(), 100);
    generic_options.add_options()
            ("output_prefix,o", po::value<path>(&output_prefix)->default_value("kat.hist"), 
                "Path prefix for files generated by this program.")
            ("threads,t", po::value<uint16_t>(&threads)->default_value(1),
                "The number of threads to use")
            ("low,l", po::value<uint64_t>(&low)->default_value(1),
                "Low count value of histogram")    
            ("high,h", po::value<uint64_t>(&high)->default_value(10000),
                "High count value of histogram")    
            ("inc,i", po::value<uint64_t>(&inc)->default_value(1),
                "Increment for each bin") 
            ("canonical,c", po::bool_switch(&canonical)->default_value(false),
                "Whether the jellyfish hashes contains K-mers produced for both strands.  If this is not set to the same value as was produced during jellyfish counting then output from sect will be unpredicatable.")
            ("mer_len,m", po::value<uint16_t>(&mer_len)->default_value(DEFAULT_MER_LEN),
                "The kmer length to use in the kmer hashes.  Larger values will provide more discriminating power between kmers but at the expense of additional memory and lower coverage.")
            ("hash_size,s", po::value<uint64_t>(&hash_size)->default_value(DEFAULT_HASH_SIZE),
                "If kmer counting is required for the input, then use this value as the hash size.  It is important this is larger than the number of distinct kmers in your set.  We do not try to merge kmer hashes in this version of KAT.")
            ("verbose,v", po::bool_switch(&verbose)->default_value(false), 
                "Print extra information.")
            ("help", po::bool_switch(&help)->default_value(false), "Produce help message.")
            ;

    // Hidden options, will be allowed both on command line and
    // in config file, but will not be shown to the user.
    po::options_description hidden_options("Hidden options");
    hidden_options.add_options()
            ("inputs,i", po::value<std::vector<path>>(&inputs), "Path to the input file(s) to process.")
            ;

    // Positional option for the input bam file
    po::positional_options_description p;
    p.add("inputs", 100);

    // Combine non-positional options
    po::options_description cmdline_options;
    cmdline_options.add(generic_options).add(hidden_options);

    // Parse command line
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(cmdline_options).positional(p).run(), vm);
    po::notify(vm);

    // Output help information the exit if requested
    if (help || argc <= 1) {
        cout << generic_options << endl;
        return 1;
    }



    auto_cpu_timer timer(1, "KAT HIST completed.\nTotal runtime: %ws\n\n");        

    cout << "Running KAT in HIST mode" << endl
         << "------------------------" << endl << endl;

    // Create the sequence coverage object
    Histogram histo(inputs, low, high, inc);
    histo.setOutputPrefix(output_prefix);
    histo.setThreads(threads);
    histo.setCanonical(canonical);
    histo.setMerLen(mer_len);
    histo.setHashSize(hash_size);
    histo.setVerbose(verbose);

    // Do the work
    histo.execute();

    // Output the results
    //histo.print(cout);

    // Send main matrix to output file
    ofstream_default main_hist_out_stream(string(output_prefix.string() + ".hist").c_str(), cout);
    histo.print(main_hist_out_stream);
    main_hist_out_stream.close();

    return 0;
}
