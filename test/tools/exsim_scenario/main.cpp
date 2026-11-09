#include <exsim/sim/scenario.hpp>
#include <test/exsim/reference/reference_engine.hpp>
#include <fstream>
#include <iostream>
#ifdef EXSIM_REFERENCE_ONLY
using Engine=exsim::ref::ReferenceEngine;
#else
using Engine=exsim::MatchingEngine;
#endif
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("usage: exsim_scenario file.scn");
        std::ifstream input(argv[1]);if(!input) throw std::runtime_error("open scenario");
        exsim::sim::run_scenario<Engine>(input);
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
