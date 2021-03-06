/**
*  @file
*  @copyright eosauthority - free to use and modify - see LICENSE.txt
*/
#include <eosio/watcher_plugin/watcher_plugin.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/trace.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/chain/block_state.hpp>

#include <fc/io/json.hpp>
#include <fc/network/url.hpp>

#include <boost/signals2/connection.hpp>
#include <boost/algorithm/string.hpp>

#include <unordered_map>
#include <zmq.hpp>
#include <string>

namespace {
  const char* SENDER_BIND = "zmq-sender-bind";
  const char* SENDER_BIND_DEFAULT = "tcp://127.0.0.1:5556";
  const uint32_t MSG_TYPE_BLOCK = 0;
  const uint32_t MSG_TYPE_IRREVERSIBLE_BLOCK = 1;
}

namespace eosio {
   static appbase::abstract_plugin& _watcher_plugin = app().register_plugin<watcher_plugin>();

   using namespace chain;


   class watcher_plugin_impl {
   public:
      typedef std::unordered_map<transaction_id_type, std::vector< action > > action_queue_t;

      static const int64_t          default_age_limit = 60;
      static const fc::microseconds http_timeout;
      static const fc::microseconds max_deserialization_time;

      struct action_notif {
         action_notif(const action& act, const variant& action_data)
         : account(act.account), name(act.name), authorization(act.authorization),
         action_data(action_data) {}

         account_name             account;
         action_name              name;
         vector<permission_level> authorization;
         fc::variant              action_data;
      };

      struct transaction {
        transaction_id_type tx_id;
        std::vector<action_notif> actions;
      };

      struct irreversible_block_message {
        uint32_t block_num;
        fc::time_point timestamp;
        uint32_t msg_type;
        std::vector<transaction_id_type> transactions;
      };

      struct  message {
        uint32_t block_num;
        fc::time_point timestamp;
        uint32_t msg_type;
        std::vector<transaction> transactions;
      };

      struct filter_entry {
         name receiver;
         name action;

         std::tuple<name, name> key() const {
            return std::make_tuple(receiver, action);
         }

         friend bool operator<( const filter_entry& a, const filter_entry& b ) {
            return a.key() < b.key();
         }
      };

      zmq::context_t context;
      zmq::socket_t sender_socket;
      chain_plugin* chain_plug = nullptr;
      fc::optional<boost::signals2::scoped_connection> accepted_block_conn;
      fc::optional<boost::signals2::scoped_connection> applied_tx_conn;
      fc::optional<boost::signals2::scoped_connection> irreversible_block_conn;
      std::set<watcher_plugin_impl::filter_entry>      filter_on;
      int64_t                                          age_limit = default_age_limit;
      action_queue_t                                   action_queue;


      watcher_plugin_impl():
        context(1),
        sender_socket(context, ZMQ_PUSH)
      {}

      bool filter( const action_trace& act, const transaction_id_type& tx_id) {  // Filter on any actions from Chintai and any actions going to Chintai
        if (
            act.act.name == "extensions" ||
            act.act.name == "undelegatebw" ||
            act.act.name == "delegatebw" ||
            act.act.name == "reminactive" ||
            act.act.name == "chinrefund" ||
            act.act.name == "delaycancel" ||
            act.act.name == "chinundel" ||
            act.act.name == "prepare" ||
            act.act.name == "activate" ||
            act.act.name == "uninit" ||
            act.act.name == "init" ||
            act.act.name == "freeze" ||
            act.act.name == "cancelorder" ||
            act.act.name == "cancelorderc" ||
            //act.act.name == "onerror" || // ignore because this won't be found in a block
            act.act.name == "processpool" ||
            act.act.name == "transfer" ||
            act.act.name == "sortdeftrx" ||
            act.act.name == "cdeferred" ||
            act.act.name == "liveundel"
            )
        {
          if (
            filter_on.find({ act.act.authorization[0].actor, 0 }) != filter_on.end() ||
            filter_on.find({ act.receipt.receiver, 0 }) != filter_on.end()
          ) {
            // Ignore invalid calls of chinundel to eosio when we accidentally broadcasted the actions to the wrong account
            if (act.act.name == "chinundel" && act.receipt.receiver == "eosio") {
              ilog("[filter] WARNING: chinundel incorrectly called on EOSIO, ignoring action and moving on. TXID: ${txid}", ("txid",tx_id));
              return false;
            }
            // ilog("Filtered true on: ${u}", ("u", act.act.name));
            return true;
          } else {
            return false;
          }
        } else {
          return false;
        }
      }

      fc::variant deserialize_action_data(action act) {
         auto& chain = chain_plug->chain();
         auto serializer = chain.get_abi_serializer(act.account, max_deserialization_time);
         FC_ASSERT(serializer.valid() &&
         serializer->get_action_type(act.name) != action_name(),
         "Unable to get abi for account: ${acc}, action: ${a} Not sending notification.",
         ("acc", act.account)("a", act.name));
         return serializer->binary_to_variant(act.name.to_string(), act.data,
         max_deserialization_time);
      }

      void on_action_trace( const action_trace& act, const transaction_id_type& tx_id ) {
        if(filter(act, tx_id)) {
          action_queue[tx_id].push_back(act.act);
          std::string data = "";
          if (!act.act.data.empty() && act.act.name != N(processpool)) {
            data = fc::json::to_string(deserialize_action_data(act.act));
          }
          ilog("[on_action_trace] [${txid}] Added trace to queue: ${action} | To: ${to} | From: ${from} | Data: ${data}", ("txid",tx_id.str().c_str())("action",act.act.name.to_string().c_str())("to",act.act.account.to_string().c_str())("from",act.act.authorization[0].actor.to_string().c_str())("data",data.c_str()));
        }

        for(const auto& iline : act.inline_traces) {
          on_action_trace(iline, tx_id);
        }
      }

      void on_applied_tx(const transaction_trace_ptr& trace) {
        if (trace->receipt) {
          // Ignore failed deferred tx that may still send an applied_transaction signal
          if( trace->receipt->status != transaction_receipt_header::executed ) {
            return;
          }

          // If we later find that a transaction was failed before it's included in a block, remove its actions from the action queue
          if (trace->failed_dtrx_trace) {
            if (action_queue.count(trace->failed_dtrx_trace->id)) {
              action_queue.erase(action_queue.find(trace->failed_dtrx_trace->id));
              return;
            }
          }

          if (action_queue.count(trace->id)) {
            ilog("[on_applied_tx] FORK WARNING: tx_id ${i} already exists -- removing existing entry before processing new actions", ("i", trace->id));
            ilog("[on_applied_tx] -------------------------------------------------------------------------------------------------------------------------------------------");
            ilog("[on_applied_tx] Previously captured tx action contents (to be removed):");
            auto range = action_queue.find(trace->id);
            for (int i = 0; i < range->second.size(); ++i) {
              std::string data = "";
              if (!range->second.at(i).data.empty() && range->second.at(i).name != N(processpool)) {
                data = fc::json::to_string(deserialize_action_data(range->second.at(i)));
              }
              ilog("[on_applied_tx] [${txid}] Action: ${action} | To: ${to} | From: ${from} | Data: ${data}", ("txid",trace->id.str().c_str())("action",range->second.at(i).name.to_string().c_str())("to",range->second.at(i).account.to_string().c_str())("from",range->second.at(i).authorization[0].actor.to_string().c_str())("data",data.c_str()));
            }
            ilog("[on_applied_tx] ==================================================================");
            ilog("[on_applied_tx] ==================================================================");
            ilog("[on_applied_tx] New trace contents to be processed for this tx:");
            for (auto at : trace->action_traces) {
              std::string data = "";
              if (!at.act.data.empty() && at.act.name != N(processpool)) {
                data = fc::json::to_string(deserialize_action_data(at.act));
              }
              ilog("[on_applied_tx] [${txid}] Action: ${action} | To: ${to} | From: ${from} | Data: ${data}", ("txid",trace->id.str().c_str())("action",at.act.name.to_string().c_str())("to",at.act.account.to_string().c_str())("from",at.act.authorization[0].actor.to_string().c_str())("data",data.c_str()));
            }
            ilog("[on_applied_tx] -------------------------------------------------------------------------------------------------------------------------------------------");
            action_queue.erase(action_queue.find(trace->id));
          }

          for (auto& at : trace->action_traces) {
             on_action_trace(at, trace->id);
          }
        }
      }

      void build_message(const transaction_id_type& tx_id, transaction& tx) {
         // ilog("inside build_message - tx_id: ${u}", ("u",tx_id));
         auto range = action_queue.find(tx_id);
         if(range == action_queue.end()) return;

         for(int i = 0; i < range->second.size(); ++i ) {
            // ilog("inside build_message for loop on iterator for action_queue range");
            // ilog("iterator range->second.at(i): ${u}", ("u",range->second.at(i).name));
            if(!range->second.at(i).data.empty() && range->second.at(i).name != N(processpool)) {
              auto act_data = deserialize_action_data(range->second.at(i));
              action_notif notif( range->second.at(i), std::forward<fc::variant>(act_data) );
              tx.actions.push_back(notif);
              // if(range->second.at(i).name == "transfer" && filter_on.find({ range->second.at(i).authorization[0].actor, 0 }) != filter_on.end() ) {
              //   i += 2;
              // }
            } else {
              variant dummy;
              action_notif notif( range->second.at(i), dummy);
              tx.actions.push_back(notif);
            }
         }
      }

      template<typename T>
      void send_zmq_message(const  T& msg) {
        // ilog("Sending: ${u}",("u",fc::json::to_string(msg)));
        string zao_json = fc::json::to_string(msg);
        zmq::message_t message(zao_json.length());
        unsigned char* ptr = (unsigned char*) message.data();
        memcpy(ptr, zao_json.c_str(), zao_json.length());
        sender_socket.send(message);
      }

      void on_accepted_block(const block_state_ptr& block_state) {
        fc::time_point btime = block_state->block->timestamp;
        if(age_limit == -1 || (fc::time_point::now() - btime < fc::seconds(age_limit))) {
          message msg;
          transaction_id_type tx_id;
          uint32_t block_num = block_state->block->block_num();
          //~ ilog("Block_num: ${u}", ("u",block_num));

          //~ Process transactions from `block_state->block->transactions` because it includes all transactions including deferred ones
          //~ ilog("Looping over all transaction objects in block_state->block->transactions");
          for( const auto& trx : block_state->block->transactions ) {
            if(trx.trx.contains<transaction_id_type>()) {
              //~ For deferred transactions the transaction id is easily accessible
              // ilog("Running: trx.trx.get<transaction_id_type>()");
              // ilog("===> block_state->block->transactions->trx ID: ${u}", ("u",trx.trx.get<transaction_id_type>()));
              tx_id = trx.trx.get<transaction_id_type>();
            } else {
              //~ For non-deferred transactions we have to access the txid from within the packed transaction. The `trx` structure and `id()` getter method are defined in `transaction.hpp`
              // ilog("Running: trx.trx.get<packed_transaction>().id()");
              // ilog("===> block_state->block->transactions->trx ID: ${u}", ("u",trx.trx.get<packed_transaction>().id()));
              tx_id = trx.trx.get<packed_transaction>().id();
            }

            if(action_queue.count(tx_id)) {
              ilog("[on_accepted_block] block_num: ${u}", ("u",block_state->block->block_num()));
              ilog("[on_accepted_block] Matched TX in accepted block: ${tx}", ("tx",tx_id));
              transaction tx;
              tx.tx_id = tx_id;
              build_message(tx_id, tx);
              msg.transactions.push_back(tx);
              action_queue.erase(action_queue.find(tx_id));
              ilog("[on_accepted_block] Action queue size after removing item: ${i}", ("i",action_queue.size()));
            }
          }

          //~ ilog("Done processing block_state->block->transactions");

          //~ Always make sure we send a new block notification to the watcher plugin for candlestick charting timestamps
          msg.block_num = block_num;
          msg.timestamp = btime;
          msg.msg_type = MSG_TYPE_BLOCK;
          send_zmq_message<message>(msg);
        }

        // Clear the queue. Any actions that were not included since the last block *should* be detected again the next time on_applied_tx is called for it
        // action_queue.clear();
      }

      void on_irreversible_block(const block_state_ptr& block_state) {
        // ilog("on_irreversible_block: ${i}", ("i", block_state->block->block_num()));
        transaction_id_type tx_id;
        irreversible_block_message msg;
        msg.block_num = block_state->block->block_num();
        msg.timestamp = block_state->block->timestamp;
        msg.msg_type = MSG_TYPE_IRREVERSIBLE_BLOCK;

        for(const auto& trx : block_state->block->transactions) {
          if(trx.trx.contains<transaction_id_type>()) {
            tx_id = trx.trx.get<transaction_id_type>();
          } else {
            tx_id = trx.trx.get<packed_transaction>().id();
          }
          msg.transactions.push_back(tx_id);
        }
        send_zmq_message<irreversible_block_message>(msg);
      }
    };

   const fc::microseconds watcher_plugin_impl::http_timeout = fc::seconds(10);
   const fc::microseconds watcher_plugin_impl::max_deserialization_time = fc::seconds(5);
   const int64_t watcher_plugin_impl::default_age_limit;

   watcher_plugin::watcher_plugin() : my(new watcher_plugin_impl()){}
   watcher_plugin::~watcher_plugin() {}

   void watcher_plugin::set_program_options(options_description&, options_description& cfg) {
      cfg.add_options()
      ("watch", bpo::value<vector<string>>()->composing(), "Track actions which match account:action. In case action is not specified, all actions of specified account are tracked.")
      ("watch-age-limit", bpo::value<int64_t>()->default_value(watcher_plugin_impl::default_age_limit), "Age limit in seconds for blocks to send notifications about. No age limit if set to negative.")
      (SENDER_BIND, bpo::value<string>()->default_value(SENDER_BIND_DEFAULT), "ZMQ Sender Socket binding");
   }

   void watcher_plugin::plugin_initialize(const variables_map& options) {
      try {
         string bind_str = options.at(SENDER_BIND).as<string>();
         if (bind_str.empty())
           {
             wlog("zmq-sender-bind not specified => eosio::watcher_plugin disabled.");
             return;
           }
         ilog("Binding to ${u}", ("u", bind_str));
         my->sender_socket.bind(bind_str);

         if (options.count("watch")) {
            auto fo = options.at("watch").as<vector<string>>();
            for (auto& s : fo) {
               // TODO: Don't require ':' for watching whole accounts
               std::vector<std::string> v;
               boost::split(v, s, boost::is_any_of(":"));
               EOS_ASSERT(v.size() == 2, fc::invalid_arg_exception,
               "Invalid value ${s} for --watch",
               ("s", s));
               watcher_plugin_impl::filter_entry fe{v[0], v[1]};
               EOS_ASSERT(fe.receiver.value, fc::invalid_arg_exception, "Invalid value ${s} for "
               "--watch", ("s", s));
               my->filter_on.insert(fe);
            }
         }

         if (options.count("watch-age-limit"))
         my->age_limit = options.at("watch-age-limit").as<int64_t>();

         my->chain_plug = app().find_plugin<chain_plugin>();
         auto& chain = my->chain_plug->chain();
         my->accepted_block_conn.emplace(chain.accepted_block.connect(
            [&](const block_state_ptr& b_state) {
               my->on_accepted_block(b_state);
         }));

         my->applied_tx_conn.emplace(chain.applied_transaction.connect(
            [&](const transaction_trace_ptr& tt) {
               my->on_applied_tx(tt);
            }
         ));

         my->irreversible_block_conn.emplace(chain.irreversible_block.connect(
            [&](const chain::block_state_ptr& b_state) {
              my->on_irreversible_block(b_state);
         }));
      } FC_LOG_AND_RETHROW()
   }

   void watcher_plugin::plugin_startup() {

   }

   void watcher_plugin::plugin_shutdown() {
      my->applied_tx_conn.reset();
      my->accepted_block_conn.reset();
      my->irreversible_block_conn.reset();
   }

}

FC_REFLECT(eosio::watcher_plugin_impl::action_notif, (account)(name)(authorization)(action_data))
FC_REFLECT(eosio::watcher_plugin_impl::message, (block_num)(timestamp)(transactions)(msg_type))
FC_REFLECT(eosio::watcher_plugin_impl::irreversible_block_message, (block_num)(timestamp)(transactions)(msg_type))
FC_REFLECT(eosio::watcher_plugin_impl::transaction, (tx_id)(actions))
