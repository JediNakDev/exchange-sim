#include <sim/journal.hpp>
#include <test/exsim/reference/reference_engine.hpp>
#include <iostream>
#ifdef EXSIM_REFERENCE_ONLY
using Engine=exsim::ref::ReferenceEngine;
#else
using Engine=exsim::MatchingEngine;
#endif
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("usage: exsim_replay journal");
        exsim::sim::JournalReader reader(argv[1]);Engine engine(reader.setup().engine);
        for(const auto& i:reader.setup().instruments) if(!engine.add_instrument(i)) throw std::runtime_error("setup");
        exsim::EventBuffer out(exsim::required_event_capacity(reader.setup().engine));
        exsim::Command c;std::uint64_t commands=0,events=0,hash=14695981039346656037ull;
        while(reader.next(c)) {out.clear();
            if(engine.handle(c,out)!=exsim::HandleResult::Applied) throw std::runtime_error("output capacity");
            ++commands;events+=out.size();for(const auto& e:out) hash=exsim::sim::hash_event(e,hash);
        }
        std::cout<<"commands="<<commands<<" events="<<events<<" hash="<<hash<<"\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
