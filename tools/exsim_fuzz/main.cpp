#include <tests/exsim/support/differential.hpp>
#include <chrono>
#include <iostream>
#ifdef EXSIM_REFERENCE_ONLY
using Engine=exsim::ref::ReferenceEngine;
#else
using Engine=exsim::MatchingEngine;
#endif
int main(int argc,char** argv) {
    try {
        std::uint64_t seed=1,seeds=40;std::size_t steps=3000;bool tiny=false,inject=false;
        std::string artifact="/tmp/exsim-divergence",replay;
        for(int i=1;i<argc;++i) {
            std::string flag=argv[i];
            if(flag=="--inject") {inject=true;continue;}
            if(i+1==argc) throw std::runtime_error("missing option value");
            std::string v=argv[++i];
            if(flag=="--seed") seed=std::stoull(v);
            else if(flag=="--seeds") seeds=std::stoull(v);
            else if(flag=="--steps") steps=std::stoull(v);
            else if(flag=="--profile") {if(v!="tiny"&&v!="mixed") throw std::runtime_error("profile: mixed|tiny");tiny=v=="tiny";}
            else if(flag=="--artifact") artifact=v;
            else if(flag=="--replay") replay=v;
            else throw std::runtime_error("unknown option");
        }
        if(!replay.empty()) {
            exsim::sim::JournalReader reader(replay);std::vector<exsim::Command> stream;exsim::Command c;
            while(reader.next(c)) stream.push_back(c);
            auto error=exsim::testsupport::replay_diff<Engine>(reader.setup(),stream);
            if(!error.empty()) {std::cerr<<error;return 1;}
            std::cout<<"replayed "<<stream.size()<<" commands without divergence\n";return 0;
        }
        auto start=std::chrono::steady_clock::now();
        exsim::ref::Mutations m;m.lifo_within_level=inject;
        for(std::uint64_t n=0;n<seeds;++n) {
            auto error=exsim::testsupport::fuzz<Engine>(seed+n,steps,tiny,m,artifact);
            if(!error.empty()) {std::cerr<<error;std::ifstream scenario(artifact+".scn");std::cerr<<"\nMinimal scenario:\n"<<scenario.rdbuf();return 1;}
        }
        auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"commands="<<seeds*steps<<" seconds="<<seconds<<" commands/s="<<seeds*steps/seconds<<"\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 2;}
}
