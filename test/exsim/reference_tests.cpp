#include <test/framework/test.hpp>
#include <test/exsim/support/differential.hpp>
#include <exsim/sim/scenario.hpp>
#include <filesystem>
#include <cstring>
#include <unistd.h>
using namespace exsim;
using namespace exsim::testsupport;
namespace {
std::string test_path(const char* name) {return "/tmp/exsim-"+std::to_string(::getpid())+"-"+name;}
}
TEST_CASE(Oracle, scenarios) {
    std::size_t count=0;
    for(const auto& entry:std::filesystem::directory_iterator("test/scenarios")) if(entry.path().extension()==".scn") {
        std::ifstream in(entry.path());
        try {sim::run_scenario<ref::ReferenceEngine>(in);}
        catch(const std::exception& e){FAIL(entry.path().string()+": "+e.what());}
        ++count;
    }
    CHECK_GE(count,15u);
}
TEST_CASE(Oracle, preflight_retry_and_ids) {
    auto setup=fuzz_setup(true);ref::ReferenceEngine a(setup.engine),b(setup.engine);
    setup_engine(a,setup);setup_engine(b,setup);
    EventBuffer small(1),out(required_event_capacity(setup.engine)),control(required_event_capacity(setup.engine));
    Event sentinel{};sentinel.type=EventType::PhaseChanged;sentinel.phase_changed={Phase::Closed,Phase::PreOpen};
    REQUIRE(small.push(sentinel));
    NewOrder n;n.instrument=1;n.session=1;n.cl_ord_id=1;n.qty=10;n.price=1000000;
    CHECK_EQ(a.handle(n,small),HandleResult::InsufficientOutput);
    CHECK_EQ(a.live_order_count(),0u);REQUIRE_EQ(small.size(),1u);CHECK(events_equal(small[0],sentinel));
    REQUIRE_EQ(a.handle(n,out),HandleResult::Applied);REQUIRE_EQ(b.handle(n,control),HandleResult::Applied);
    REQUIRE_EQ(out.size(),control.size());
    for(std::size_t i=0;i<out.size();++i) CHECK(events_equal(out[i],control[i]));
    for(const Command c:{Command{ReplaceOrder{0,1,1,2,1000000,8,true}},
                        Command{CancelOrder{0,1,1,3,2}},Command{MassCancel{0,1}},
                        Command{SetPhase{0,1,Phase::Closed}}}) {
        CHECK_EQ(a.handle(c,small),HandleResult::InsufficientOutput);CHECK(book_diff(a,b).empty());
        out.clear();control.clear();REQUIRE_EQ(a.handle(c,out),HandleResult::Applied);
        REQUIRE_EQ(b.handle(c,control),HandleResult::Applied);
        REQUIRE_EQ(out.size(),control.size());for(std::size_t i=0;i<out.size();++i) CHECK(events_equal(out[i],control[i]));
    }
}
TEST_CASE(Oracle, config_and_wide_quantities) {
    bool threw=false;try {ref::ReferenceEngine e({0,1});}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
    ref::ReferenceEngine e({1,4});InstrumentConfig i;i.max_order_qty=UINT32_MAX;i.symbol=Symbol("WIDE");
    i.initial_phase=Phase::PreOpen;i.reference_price=i.min_price;
    REQUIRE_EQ(e.add_instrument(i),1);i.reference_price=0;CHECK_EQ(e.add_instrument(i),0);
    EventBuffer out(required_event_capacity({1,4}));
    for(ClOrdId token=1;token<=2;++token) {
        NewOrder n;n.instrument=1;n.session=1;n.cl_ord_id=token;n.qty=UINT32_MAX;n.price=100;
        out.clear();REQUIRE_EQ(e.handle(n,out),HandleResult::Applied);
    }
    CHECK_EQ(e.auction_indicative(1).imbalance_qty,2ull*UINT32_MAX);
    LevelInfo level{};REQUIRE_EQ(e.depth(1,Side::BUY,&level,1),1u);CHECK_EQ(level.qty,2ull*UINT32_MAX);
    for(ClOrdId token=3;token<=4;++token) {
        NewOrder n;n.instrument=1;n.session=1;n.cl_ord_id=token;n.qty=UINT32_MAX;n.price=100;n.side=Side::SELL;
        out.clear();REQUIRE_EQ(e.handle(n,out),HandleResult::Applied);
    }
    CHECK_EQ(e.auction_indicative(1).paired_qty,2ull*UINT32_MAX);
    out.clear();REQUIRE_EQ(e.handle(SetPhase{0,1,Phase::Continuous},out),HandleResult::Applied);
    REQUIRE_EQ(out.size(),4u);CHECK_EQ(out[2].type,EventType::AuctionSummary);
    CHECK_EQ(out[2].auction_summary.matched_qty,2ull*UINT32_MAX);CHECK_EQ(out[2].auction_summary.match_id,3u);

}
TEST_CASE(Oracle, deterministic_sweep) {
    for(std::uint64_t seed=1;seed<=40;++seed) {
        auto error=fuzz<ref::ReferenceEngine>(seed,3000,seed%2==0);
        if(!error.empty()) FAIL(error);
    }
}
TEST_CASE(Oracle, detects_and_shrinks_fifo_mutation) {
    ref::Mutations m;m.lifo_within_level=true;
    auto error=fuzz<ref::ReferenceEngine>(1,10000,false,m,test_path("injected-fifo"));
    REQUIRE(!error.empty());
    sim::JournalReader reader(test_path("injected-fifo.journal"));std::vector<Command> commands;Command c;
    while(reader.next(c)) commands.push_back(c);
    CHECK(!replay_diff<ref::ReferenceEngine>(reader.setup(),commands,m).empty());
    for(std::size_t j=0;j<commands.size();++j) {
        auto candidate=commands;candidate.erase(candidate.begin()+static_cast<std::ptrdiff_t>(j));
        CHECK(replay_diff<ref::ReferenceEngine>(reader.setup(),candidate,m).empty());
    }
    std::ifstream in(test_path("injected-fifo.scn"));sim::run_scenario<ref::ReferenceEngine>(in);
}
TEST_CASE(Journal, roundtrip_hash_and_corruption) {
    auto setup=fuzz_setup(true);ref::ReferenceEngine engine(setup.engine);setup_engine(engine,setup);
    EventBuffer out(required_event_capacity(setup.engine));CommandGenerator gen(192);
    std::vector<Command> commands;std::uint64_t hash=14695981039346656037ull;
    {sim::JournalWriter writer(test_path("roundtrip.journal"),setup);
        for(unsigned j=0;j<10000;++j) {
            auto c=gen.next(engine);commands.push_back(c);REQUIRE(writer.append(c));if(j%200==0) REQUIRE(writer.flush());out.clear();
            REQUIRE_EQ(engine.handle(c,out),HandleResult::Applied);for(const auto& e:out) hash=sim::hash_event(e,hash);
        }REQUIRE(writer.flush());
    }
    sim::JournalReader reader(test_path("roundtrip.journal"));ref::ReferenceEngine replay(reader.setup().engine);
    setup_engine(replay,reader.setup());Command c;std::size_t index=0;std::uint64_t replay_hash=14695981039346656037ull;
    while(reader.next(c)) {
        REQUIRE(index<commands.size());CHECK(commands_equal(c,commands[index++]));out.clear();
        REQUIRE_EQ(replay.handle(c,out),HandleResult::Applied);for(const auto& e:out) replay_hash=sim::hash_event(e,replay_hash);
    }
    CHECK_EQ(index,commands.size());CHECK_EQ(hash,replay_hash);
    std::ifstream input(test_path("roundtrip.journal"),std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(input),{}};
    for(auto n:{0u,8u,39u,100u,static_cast<unsigned>(bytes.size()-1),static_cast<unsigned>(bytes.size()-280),static_cast<unsigned>(bytes.size()-24)}) {
        std::ofstream corrupt(test_path("corrupt.journal"),std::ios::binary);corrupt.write(bytes.data(),n);corrupt.close();
        bool threw=false;try{sim::JournalReader bad(test_path("corrupt.journal"));}catch(const std::exception&){threw=true;}CHECK(threw);
    }
    for(auto pos:{0u,8u,32u,50u,static_cast<unsigned>(bytes.size()-1)}) {
        bytes[pos]^=1;std::ofstream corrupt(test_path("corrupt.journal"),std::ios::binary);
        corrupt.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));corrupt.close();bytes[pos]^=1;
        bool threw=false;try{sim::JournalReader bad(test_path("corrupt.journal"));}catch(const std::exception&){threw=true;}CHECK(threw);
    }
}
TEST_CASE(Journal, active_fields_ignore_padding) {
    for(auto type:sim::kAllEventTypes) {
        Event a{},b{};std::memset(static_cast<void*>(&a),0x55,sizeof(a));std::memset(static_cast<void*>(&b),0xaa,sizeof(b));
        a.type=type;b.type=type;a.instrument=b.instrument=1;a.ts=b.ts=7;
        // Initialize every active payload from the same value while preserving distinct inactive bytes.
        switch(type) {
            case EventType::Accepted:a.accepted=b.accepted=AcceptedEvent{};break;
            case EventType::Rejected:a.rejected=b.rejected=RejectedEvent{};break;
            case EventType::Rested:a.rested=b.rested=RestedEvent{};break;
            case EventType::Fill:a.fill=b.fill=FillEvent{};break;
            case EventType::Canceled:a.canceled=b.canceled=CanceledEvent{};break;
            case EventType::Replaced:a.replaced=b.replaced=ReplacedEvent{};break;
            case EventType::MassCancelDone:a.mass_cancel_done=b.mass_cancel_done=MassCancelDoneEvent{};break;
            case EventType::PhaseChanged:a.phase_changed=b.phase_changed=PhaseChangedEvent{};break;
            case EventType::AuctionSummary:a.auction_summary=b.auction_summary=AuctionSummaryEvent{};break;
        }
        CHECK(events_equal(a,b));CHECK_EQ(sim::hash_event(a),sim::hash_event(b));
    }
    Event a{};a.type=EventType::Canceled;a.canceled=CanceledEvent{};auto b=a;b.canceled.leaves_qty=1;
    CHECK(!events_equal(a,b));CHECK_NE(sim::hash_event(a),sim::hash_event(b));
    a.type=EventType::AuctionSummary;a.auction_summary=AuctionSummaryEvent{};b=a;b.auction_summary.match_id=1;
    CHECK(!events_equal(a,b));CHECK_NE(sim::hash_event(a),sim::hash_event(b));
}
TEST_CASE(Oracle, random_state_invariants) {
    for(std::uint64_t seed=1;seed<=10;++seed) {
        auto setup=fuzz_setup(seed%2==0);ref::ReferenceEngine engine(setup.engine);setup_engine(engine,setup);
        CommandGenerator gen(seed);EventBuffer out(required_event_capacity(setup.engine));
        MatchId last_match=0;OrderId max_id=0;
        for(unsigned step=0;step<3000;++step) {
            auto c=gen.next(engine);out.clear();REQUIRE_EQ(engine.handle(c,out),HandleResult::Applied);
            for(const auto& e:out) {
                CHECK_EQ(e.ts,c.ts());
                if(e.type==EventType::Accepted) {CHECK_GT(e.accepted.order_id,max_id);max_id=e.accepted.order_id;}
                if(e.type==EventType::Replaced && !e.replaced.priority_kept) {CHECK_GT(e.replaced.new_order_id,max_id);max_id=e.replaced.new_order_id;}
                if(e.type==EventType::Fill) {
                    CHECK_EQ(e.fill.match_id,++last_match);CHECK_GT(e.fill.qty,0u);
                    CHECK_GT(e.fill.buy.cum_qty,0u);CHECK_GT(e.fill.sell.cum_qty,0u);
                }
                if(e.type==EventType::AuctionSummary) CHECK_EQ(e.auction_summary.match_id,++last_match);
            }
            std::size_t live=0;OrderId previous=0;
            engine.for_each_order([&](const OrderInfo& o) {
                CHECK_GT(o.order_id,previous);previous=o.order_id;++live;
                CHECK_GT(o.leaves_qty,0u);CHECK_EQ(static_cast<std::uint64_t>(o.leaves_qty)+o.cum_qty,o.qty);
                CHECK(order_infos_equal(o,*engine.find_order(o.order_id)));
                CHECK(order_infos_equal(o,*engine.find_order(o.session,o.cl_ord_id)));
            });
            CHECK_EQ(live,engine.live_order_count());
            for(InstrumentId id=1;id<=3;++id) {
                auto bid=engine.best_bid(id),ask=engine.best_ask(id);
                if((engine.phase(id)==Phase::Closed || engine.phase(id)==Phase::Continuous) && bid && ask) CHECK_LT(*bid,*ask);
                for(auto side:{Side::BUY,Side::SELL}) {
                    std::vector<LevelInfo> levels(live+1);auto n=engine.depth(id,side,levels.data(),levels.size());
                    for(std::size_t j=0;j<n;++j) {
                        CHECK_GT(levels[j].qty,0u);CHECK_GT(levels[j].order_count,0u);
                        std::vector<OrderId> ids(live+1);
                        auto count=engine.level_orders(id,side,levels[j].price,ids.data(),ids.size());
                        CHECK_EQ(count,levels[j].order_count);
                        std::uint64_t sum=0;for(std::size_t k=0;k<count;++k) {
                            auto o=engine.find_order(ids[k]);REQUIRE(o.has_value());
                            sum+=o->leaves_qty;CHECK_EQ(o->side,side);CHECK_EQ(o->price,levels[j].price);
                            if(k) CHECK_LT(ids[k-1],ids[k]);
                        }
                        CHECK_EQ(sum,levels[j].qty);
                    }
                }
            }
        }
    }
}
TEST_CASE(Journal, every_command_field_roundtrips) {
    std::vector<Command> commands{
        NewOrder{UINT64_MAX,65535,65535,UINT32_MAX,kMaxClOrdId,Side::SELL,OrdType::Market,TimeInForce::FOK,StpMode::CancelBoth,true,INT64_MIN,UINT32_MAX},
        CancelOrder{UINT64_MAX,65535,kMaxClOrdId,UINT64_MAX,UINT32_MAX},
        ReplaceOrder{UINT64_MAX,65535,kMaxClOrdId,UINT64_MAX,INT64_MIN,UINT32_MAX,true},
        MassCancel{UINT64_MAX,65535,65535,UINT64_MAX,true},
        SetPhase{UINT64_MAX,65535,Phase::PreClose}
    };
    for(const auto& c:commands) {
        auto bytes=sim::encode(c);auto decoded=sim::decode_command(bytes);
        CHECK(commands_equal(c,decoded));CHECK_EQ(sim::encode(decoded),bytes);
    }
    auto c=commands[1],d=c;d.cancel.reduce_to_total_qty=0;CHECK(!commands_equal(c,d));CHECK_NE(sim::encode(c),sim::encode(d));
    c=commands[2];d=c;d.replace.force_priority_loss=false;CHECK(!commands_equal(c,d));CHECK_NE(sim::encode(c),sim::encode(d));
    CHECK_EQ(sim::parse_price("-922337203685477.5808"),std::optional<Price>{INT64_MIN});
    CHECK(!sim::parse_price("922337203685477.5808"));
}
TEST_CASE(Oracle, amendment_chain_conservation) {
    struct Account {std::int64_t supply=0,executed=0,canceled=0;};
    auto setup=fuzz_setup();ref::ReferenceEngine engine(setup.engine);setup_engine(engine,setup);
    CommandGenerator gen(481);EventBuffer out(required_event_capacity(setup.engine));
    std::map<OrderId,Account> accounts;
    for(unsigned step=0;step<10000;++step) {
        auto command=gen.next(engine);
        std::map<OrderId,Quantity> previous;
        engine.for_each_order([&](const OrderInfo& o){previous[o.order_id]=o.qty;});
        out.clear();REQUIRE_EQ(engine.handle(command,out),HandleResult::Applied);
        for(const auto& e:out) {
            if(e.type==EventType::Accepted) accounts[e.accepted.order_id].supply=e.accepted.qty;
            if(e.type==EventType::Replaced) {
                const auto& r=e.replaced;auto account=accounts.at(r.old_order_id);
                account.supply+=static_cast<std::int64_t>(r.qty)-previous.at(r.old_order_id);
                accounts.erase(r.old_order_id);accounts[r.new_order_id]=account;
            }
            if(e.type==EventType::Fill) {
                accounts.at(e.fill.buy.order_id).executed+=e.fill.qty;
                accounts.at(e.fill.sell.order_id).executed+=e.fill.qty;
            }
            if(e.type==EventType::Canceled) accounts.at(e.canceled.order_id).canceled+=e.canceled.canceled_qty;
        }
        for(const auto& [id,a]:accounts) {
            auto order=engine.find_order(id);
            CHECK_EQ(a.supply,a.executed+a.canceled+(order?order->leaves_qty:0));
        }
    }
}
#include <test/exsim/support/contract_gates.hpp>
TEST_CASE(Oracle, auction_retry_counters) {
    check_auction_retry<ref::ReferenceEngine>();
}
TEST_CASE(Oracle, reduction_fifo_all_phases) {
    check_reduction_fifo_all_phases<ref::ReferenceEngine>();
}
