#pragma once

#include <assert.h>
#include <condition_variable>
#include <experimental/optional>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <ostream>
#include <list>
#include <set>
#include <tuple>
#include <vector>

#include "connection_manager.h"
#include "derecho_sst.h"
#include "filewriter.h"
#include "mutils-serialization/SerializationMacros.hpp"
#include "mutils-serialization/SerializationSupport.hpp"
#include "rdmc/rdmc.h"
#include "sst/sst.h"

namespace derecho {

/** Alias for the type of std::function that is used for message delivery event callbacks. */
using message_callback = std::function<void(uint32_t, int, long long int, char*, long long int)>;

using rpc_handler_t = std::function<void(node_id_t, char*, uint32_t)>;

/**
 * Bundles together a set of callback functions for message delivery events.
 * These will be invoked by DerechoGroup to hand control back to the client
 * when it needs to handle message delivery.
 */
struct CallbackSet {
    message_callback global_stability_callback;
    message_callback local_persistence_callback = nullptr;
};

struct DerechoParams : public mutils::ByteRepresentable {
    long long unsigned int max_payload_size;
    long long unsigned int block_size;
    std::string filename = std::string();
    unsigned int window_size = 3;
    unsigned int timeout_ms = 1;
    rdmc::send_algorithm type = rdmc::BINOMIAL_SEND;
    uint32_t rpc_port = 12487;

    DerechoParams(long long unsigned int max_payload_size,
                  long long unsigned int block_size,
                  std::string filename = std::string(),
                  unsigned int window_size = 3,
                  unsigned int timeout_ms = 1,
                  rdmc::send_algorithm type = rdmc::BINOMIAL_SEND,
                  uint32_t rpc_port = 12487)
            : max_payload_size(max_payload_size),
              block_size(block_size),
              filename(filename),
              window_size(window_size),
              timeout_ms(timeout_ms),
              type(type),
              rpc_port(rpc_port) {
    }

    DEFAULT_SERIALIZATION_SUPPORT(DerechoParams, max_payload_size, block_size, filename, window_size, timeout_ms, type, rpc_port);
};

struct __attribute__((__packed__)) header {
    uint32_t header_size;
    uint32_t pause_sending_turns;
    bool cooked_send;
};

/**
 * Represents a block of memory used to store a message. This object contains
 * both the array of bytes in which the message is stored and the corresponding
 * RDMA memory region (which has registered that array of bytes as its buffer).
 * This is a move-only type, since memory regions can't be copied.
 */
struct MessageBuffer {
    std::unique_ptr<char[]> buffer;
    std::shared_ptr<rdma::memory_region> mr;

    MessageBuffer() {}
    MessageBuffer(size_t size) {
        if(size != 0) {
            buffer = std::unique_ptr<char[]>(new char[size]);
            mr = std::make_shared<rdma::memory_region>(buffer.get(), size);
        }
    }
    MessageBuffer(const MessageBuffer&) = delete;
    MessageBuffer(MessageBuffer&&) = default;
    MessageBuffer& operator=(const MessageBuffer&) = delete;
    MessageBuffer& operator=(MessageBuffer&&) = default;
};

struct Message {
    /** The rank of the message's sender within this group. */
    int sender_rank;
    /** The message's index (relative to other messages sent by that sender). */
    long long int index;
    /** The message's size in bytes. */
    long long unsigned int size;
    /** The MessageBuffer that contains the message's body. */
    MessageBuffer message_buffer;
};

/** Implements the low-level mechanics of tracking multicasts in a Derecho group,
 * using RDMC to deliver messages and SST to track their arrival and stability.
 * This class should only be used as part of a Group, since it does not know how
 * to handle failures. */
class MulticastGroup {
private:
    /** vector of member id's */
    std::vector<node_id_t> members;
    /** inverse map of node_ids to sst_row */
    std::map<node_id_t, uint32_t> node_id_to_sst_index;
    /**  number of members */
    const int num_members;
    /** index of the local node in the members vector, which should also be its row index in the SST */
    const int member_index;
public:    //consts can be public, right?
    /** Block size used for message transfer.
     * we keep it simple; one block size for messages from all senders */
    const long long unsigned int block_size;
    // maximum size of any message that can be sent
    const long long unsigned int max_msg_size;
    /** Send algorithm for constructing a multicast from point-to-point unicast.
     *  Binomial pipeline by default. */
    const rdmc::send_algorithm type;
    const unsigned int window_size;
private:
    /** Message-delivery event callbacks, supplied by the client, for "raw" sends */
    const CallbackSet callbacks;
    uint32_t total_num_subgroups;
    const std::map<uint32_t, std::pair<uint32_t, uint32_t>> subgroup_to_shard_n_index;
    const std::map<uint32_t, uint32_t> subgroup_to_num_received_offset;
    const std::map<uint32_t, std::vector<node_id_t>> subgroup_to_membership;
    std::map<uint32_t, uint32_t> subgroup_to_rdmc_group;
    /** These two callbacks are internal, not exposed to clients, so they're not in CallbackSet */
    rpc_handler_t rpc_callback;

    /** Offset to add to member ranks to form RDMC group numbers. */
    uint16_t rdmc_group_num_offset;
    /** false if RDMC groups haven't been created successfully */
    bool rdmc_groups_created = false;
    /** Stores message buffers not currently in use. Protected by
     * msg_state_mtx */
    std::map<uint32_t, std::vector<MessageBuffer>> free_message_buffers;


    /** Index to be used the next time get_sendbuffer_ptr is called.
     * When next_message is not none, then next_message.index = future_message_index-1 */
    std::vector<long long int> future_message_indices;

       /** next_message is the message that will be sent when send is called the next time.
     * It is boost::none when there is no message to send. */
    std::vector<std::experimental::optional<Message>> next_sends;
    /** Messages that are ready to be sent, but must wait until the current send finishes. */
    std::vector<std::queue<Message>> pending_sends;
    /** Vector of messages that are currently being sent out using RDMC, or boost::none otherwise. */
    /** one per subgroup */
    std::vector<std::experimental::optional<Message>> current_sends;

    /** Messages that are currently being received. */
    std::map<std::pair<uint32_t, long long int>, Message> current_receives;

    /** Messages that have finished sending/receiving but aren't yet globally stable */
    std::map<uint32_t, std::map<long long int, Message>> locally_stable_messages;
    /** Messages that are currently being written to persistent storage */
    std::map<uint32_t, std::map<long long int, Message>> non_persistent_messages;

    std::vector<long long int> next_message_to_deliver;
    std::mutex msg_state_mtx;
    std::condition_variable sender_cv;

    /** The time, in milliseconds, that a sender can wait to send a message before it is considered failed. */
    unsigned int sender_timeout;

    /** Indicates that the group is being destroyed. */
    std::atomic<bool> thread_shutdown{false};
    /** The background thread that sends messages with RDMC. */
    std::thread sender_thread;

    std::thread timeout_thread;

    /** The SST, shared between this group and its GMS. */
    std::shared_ptr<DerechoSST> sst;

    using pred_handle = typename sst::Predicates<DerechoSST>::pred_handle;
    pred_handle stability_pred_handle;
    pred_handle delivery_pred_handle;
    pred_handle sender_pred_handle;

    std::unique_ptr<FileWriter> file_writer;

    /** Continuously waits for a new pending send, then sends it. This function
     * implements the sender thread. */
    void send_loop();

    /** Checks for failures when a sender reaches its timeout. This function
     * implements the timeout thread. */
    void check_failures_loop();

    std::function<void(persistence::message)> make_file_written_callback();
    bool create_rdmc_groups();
    void initialize_sst_row();
    void register_predicates();

    void deliver_message(Message& msg, uint32_t subgroup_num);

public:
    // the constructor - takes the list of members, send parameters (block size, buffer size), K0 and K1 callbacks
    MulticastGroup(
        std::vector<node_id_t> _members, node_id_t my_node_id,
        std::shared_ptr<DerechoSST> _sst,
        CallbackSet callbacks,
        uint32_t total_num_subgroups,
        const std::map<uint32_t, std::pair<uint32_t, uint32_t>>& subgroup_to_shard_n_index,
        const std::map<uint32_t, uint32_t>& subgroup_to_num_received_offset,
        const std::map<uint32_t, std::vector<node_id_t>>& subgroup_to_membership,
        const DerechoParams derecho_params,
        std::vector<char> already_failed = {});
    /** Constructor to initialize a new MulticastGroup from an old one,
     * preserving the same settings but providing a new list of members. */
    MulticastGroup(
        std::vector<node_id_t> _members, node_id_t my_node_id,
        std::shared_ptr<DerechoSST> _sst,
        MulticastGroup&& old_group,
        uint32_t total_num_subgroups,
        const std::map<uint32_t, std::pair<uint32_t, uint32_t>>& subgroup_to_shard_n_index,
        const std::map<uint32_t, uint32_t>& subgroup_to_num_received_offset,
        const std::map<uint32_t, std::vector<node_id_t>>& subgroup_to_membership,
        std::vector<char> already_failed = {}, uint32_t rpc_port = 12487);
    ~MulticastGroup();

    /**
     * Registers a function to be called upon receipt of a multicast RPC message
     * @param handler A function that will handle RPC messages.
     */
    void register_rpc_callback(rpc_handler_t handler) { rpc_callback = std::move(handler); }

    void deliver_messages_upto(const std::vector<long long int>& max_indices_for_senders, uint32_t subgroup_num, uint32_t num_shard_members);
    /** Get a pointer into the current buffer, to write data into it before sending */
    char* get_sendbuffer_ptr(uint32_t subgroup_num, long long unsigned int payload_size,
                             int pause_sending_turns = 0, bool cooked_send = false);
    /** Note that get_position and send are called one after the another - regexp for using the two is (get_position.send)*
     * This still allows making multiple send calls without acknowledgement; at a single point in time, however,
     * there is only one message per sender in the RDMC pipeline */
    bool send(uint32_t subgroup_num);

    /** Stops all sending and receiving in this group, in preparation for shutting it down. */
    void wedge();
    /** Debugging function; prints the current state of the SST to stdout. */
    void debug_print();
    static long long unsigned int compute_max_msg_size(
        const long long unsigned int max_payload_size,
        const long long unsigned int block_size);
    const std::map<uint32_t, std::pair<uint32_t, uint32_t>>& get_subgroup_to_shard_n_index() {
        return subgroup_to_shard_n_index;
    }
    const std::map<uint32_t, uint32_t>& get_subgroup_to_num_received_offset() {
        return subgroup_to_num_received_offset;
    }
    std::vector<uint32_t> get_shard_sst_indices(uint32_t subgroup_num);
};
}  // namespace derecho