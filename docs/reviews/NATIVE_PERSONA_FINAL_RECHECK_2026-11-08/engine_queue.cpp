// Independent scenario: real Simulation and MatchingEngine emissions, production canonical projection.
#include "exsim/research/simulation.hpp"
#include "exsim/api/request_diagnostics.hpp"
#include "exsim/api/api_json.hpp"
#include "exsim/research/json.hpp"
#include <cassert>
#include <iostream>
#include <vector>
using namespace exsim;
using namespace exsim::research;
using namespace exsim::api;
struct QueueMaker {
 static constexpr std::string_view kStrategyName="independent_queue_maker";
 static constexpr std::uint32_t kStrategyVersion=1;
 ClOrdId token=0;int phase=0;
 void on_start(StrategyContext& c){c.wake_at(1000000);}
 void on_market_data(const MarketDataUpdate&,StrategyContext&){}
 void on_execution_report(const ExecutionReport&,StrategyContext&){}
 void on_wakeup(StrategyContext& c){
  if(phase==0){token=c.submit_limit_order(Side::BUY,999900,10,TimeInForce::Day,true);assert(token);c.wake_at(2000000);}
  if(phase==1){assert(c.replace_order(token,1000000,10));c.wake_at(3000000);}
  if(phase==2){assert(c.replace_order(token,1000000,5));}
  ++phase;
 }
};
int main(){
 auto c=default_experiment_config();c.agents.clear();c.limits.duration_ns=10000000;
 c.latency.feed_ns=0;c.latency.order_ns=0;c.latency.cancel_ns=0;c.latency.ack_ns=0;c.latency.strategy_compute_ns=0;c.latency.feed_jitter_ns=0;c.latency.order_jitter_ns=0;c.latency.cancel_jitter_ns=0;c.latency.ack_jitter_ns=0;
 validate_experiment_config(c);
 Simulation<QueueMaker> sim(c,103); CanonicalEventBuilder builder(c);std::vector<CanonicalEventRecord> events;
 NewOrder ahead;ahead.instrument=1;ahead.session=2;ahead.cl_ord_id=1;ahead.side=Side::BUY;ahead.price=1000000;ahead.qty=30;
 assert(sim.schedule_scripted_command(0,ahead));
 std::size_t nc=0,nl=0;std::uint64_t seq=0;bool checked40=false,checked35=false;
 for(int j=0;j<2000;++j){auto stop=sim.step();auto& rec=sim.recorder();
 builder.append_step(sim,rec.commands.records().subspan(nc),rec.lifecycle.records().subspan(nl),++seq,events);nc=rec.commands.size();nl=rec.lifecycle.size();
 assert(builder.consistency_error().empty());
 if(sim.now()==2000000||sim.now()==3000000){
  const auto o=sim.engine().find_order(kStrategySessionId,sim.strategy().token);
  if(o&&o->price==1000000){OrderId q[4];assert(sim.engine().level_orders(1,Side::BUY,1000000,q,4)==2);assert(q[1]==o->order_id);assert(sim.engine().find_order(q[0])->leaves_qty==30);
   if(o->leaves_qty==10)checked40=true;if(o->leaves_qty==5)checked35=true;
  }
 }

 if(stop==RunStop::Completed)break;
 assert(stop==RunStop::StepBudgetExhausted || stop==RunStop::Continue);
 }
 std::vector<OrderId> ids;bool lost=false,kept=false;OrderId old=0,newid=0;
 for(const auto& e:events)if(e.kind==CanonicalEventKind::OrderReplaced){ids.push_back(e.replaced_old_order_id);ids.push_back(e.order_id);if(e.has(kPriorityKept))kept=true;else {lost=true;old=e.replaced_old_order_id;newid=e.order_id;}}
 assert(lost&&kept&&old!=newid);
 ApiJsonWriter json;assert(!write_fifo_evidence(json,events,ids,events.size()-1));auto output=json.take();auto rows=parse_json(output);bool rest=false,reduction=false,reset=false;
 for(const auto& x:rows.items){auto reason=x.find("reason")->text;
  if(reason=="replace_priority_reset"){assert(!x.find("queuePositionAvailable")->boolean);assert(x.find("sharesAhead")->kind==JsonValue::Kind::Null);reset=true;}
  if(reason=="rested"&&x.find("orderId")->text==std::to_string(newid)){assert(x.find("sharesAhead")->text=="30");assert(x.find("ordersAhead")->text=="1");assert(x.find("levelTotalShares")->text=="40");rest=true;}
  if(reason=="replace_priority_kept"){assert(x.find("sharesAhead")->text=="30");assert(x.find("levelTotalShares")->text=="35");reduction=true;}
 }
 // Independent direct native engine query corroborates the projection's final snapshot.
 const auto& engine=builder.shadow_engine();OrderId fifo[4];auto count=engine.level_orders(1,Side::BUY,1000000,fifo,4);assert(count==2&&fifo[1]==newid);
 assert(engine.find_order(newid)->leaves_qty==5);assert(engine.find_order(fifo[0])->leaves_qty==30);
 assert(rest&&reduction&&reset&&checked40&&checked35);std::cout<<output<<'\n';
 // Deliberate boundary-only projection inputs, separate from the actual queue scenario.
 for(OrderId target:{OrderId{7},OrderId{0}}){
  std::vector<CanonicalEventRecord> boundary(3);boundary[0].kind=CanonicalEventKind::StrategyReplaceSubmitted;boundary[0].order_id=target;
  boundary[1].kind=CanonicalEventKind::OrderFilled;boundary[1].order_id=999;boundary[1].buy_leg_order_id=999;boundary[1].sell_leg_order_id=0;
  boundary[2].kind=CanonicalEventKind::ReplaceArrivedAtExchange;boundary[2].order_id=0;
  std::vector<std::uint64_t> positions{0,1,2};ApiJsonWriter w;write_replace_transits(w,boundary,positions);const auto b=parse_json(w.take());assert(b.items[0].find("fillBeforeArrivalEventPosition")->kind==JsonValue::Kind::Null);
 }
 for(std::size_t j=0;j<events.size();++j){ApiJsonWriter w;write_canonical_event_json(w,events[j],j);std::cout<<w.take()<<'\n';}
}
