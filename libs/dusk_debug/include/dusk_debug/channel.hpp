/**
 * \file channel.hpp
 * \brief Bounded lock-free MPMC channel based on Dmitry Vyukov's "Bounded MPMC queue" algorithm
 *
 * \author Brenden Davidson <brenden@bdavidson.dev>
 * \date 2026-08-10
 */

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include <tracy/Tracy.hpp>

namespace dusk {

namespace detail {
static constexpr size_t T_NAME_LEN = 16;
static constexpr size_t T_PLOT_NAME_LEN = T_NAME_LEN + 12;

enum class ChannelState {
    Open,
    DrainOnly,
    Closed,
};
}  // namespace detail

/**
 * \brief Bounded capacity multi-producer, single consumer (MPSC) channel.
 *
 * \remark Users MUST use the `make_channel` static method to get a Sender/Receiver pair instead of trying to construct
 * this class directly.
 *
 * \remark The current Sender/Receiver implementation is what actually restricts channel usage to single consumer. The
 * underlying architecture supports multi-consumer workloads with a small performance penalty,
 *
 * \tparam CAPACITY upper limit of the number of messages the Channel can hold before dropping.
 * Must be non-zero and a power of 2.
 *
 * \tparam T data type which can be sent via this channel (must conform to std::movable)
 */
template <size_t CAPACITY, std::movable T>
class Channel {
    static_assert(CAPACITY >= 2 && std::has_single_bit(CAPACITY),
        "Channel must have a non-zero capacity that is a power of 2");

    static constexpr size_t MASK = CAPACITY - 1;

    /**
     * \brief Message sent via a channel
     */
    class Message {
        alignas(std::hardware_destructive_interference_size) std::atomic_size_t m_sequence{};
        std::atomic_bool m_has_waiter{false};
        std::optional<T> m_payload{std::nullopt};

    public:
        Message() noexcept = default;

        ~Message() noexcept {
            // Pretend that the payload was consumed by updating the sequence value.
            this->m_sequence.fetch_add(MASK, std::memory_order_seq_cst);
            this->m_sequence.notify_one();
        };

        Message(const Message&&) noexcept = delete;
        Message(Message&&) noexcept = delete;

        /**
         * Consumes the Message's payload, notifying any senders waiting for confirmation of
         * receipt.
         *
         * @return the Message's payload
         */
        [[nodiscard]] std::optional<T> consume() noexcept {
            std::optional<T> payload{std::nullopt};
            this->m_payload.swap(payload);

            this->m_sequence.fetch_add(MASK, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (this->m_has_waiter.load(std::memory_order_seq_cst)) {
                this->m_sequence.notify_one();
            }
            return payload;
        }

        /**
         * Assigns a new payload to this Message, consuming and dropping any existing one in the
         * process
         *
         * @param payload new payload
         */
        void set_payload(T payload) noexcept {
            this->m_payload = std::move(payload);

            this->m_sequence.fetch_add(1, std::memory_order_release);
        }

        [[nodiscard]] size_t get_sequence() const noexcept {
            return this->m_sequence.load(std::memory_order_acquire);
        }

        /**
         *
         * @internal
         * @param seq
         */
        void set_sequence(const size_t seq) noexcept {
            this->m_sequence.store(seq, std::memory_order_release);
        }

        void await_recv(const size_t current_pos) noexcept {
            this->m_has_waiter.store(true, std::memory_order_seq_cst);
            this->m_sequence.wait(current_pos, std::memory_order_acquire);
            this->m_has_waiter.store(false, std::memory_order_relaxed);
        }
    };

    /// Backing Message arena.
    ///
    /// \remarks The `Message` class is already cache-aligned for performance. Therefore, the array
    /// inherits that alignment.
    std::array<Message, CAPACITY> m_msg_arena;

    // =============================
    // BEGIN: Bookkeeping cache line
    // =============================
    //
    // Both producers and consumers may read and/or modify these values.

    /// Flag indicating if the channel has been closed and cannot accept new messages.
    alignas(std::hardware_destructive_interference_size) std::atomic<detail::ChannelState> m_state{
        detail::ChannelState::Open};
    std::atomic_size_t m_sender_count{0};
    std::atomic_size_t m_receiver_count{0};

    // ==========================
    // BEGIN: Producer cache line
    // ==========================

    alignas(std::hardware_destructive_interference_size) std::atomic_size_t m_tail{0};
    /// Count of dropped messages over this Channel's lifetime
    std::atomic_size_t m_dropped{0};
    /// Count of senders waiting for messages to be received.
    std::atomic_size_t m_wait_count{0};

    // ==========================
    // BEGIN: Consumer cache line
    // ==========================

    alignas(std::hardware_destructive_interference_size) std::atomic_size_t m_head{0};

    /// Flag indicating whether channel stats should be reported to Tracy
    bool m_report_enabled;

    /// This Channel's static Tracy reporting strings
    ///
    /// \remarks Currently these are not held static for the life of the program as Tracy would
    /// prefer. This has the possibility of creating conflicting reports if a Channel is destroyed
    /// and another with the same name is created.
    std::array<char, detail::T_NAME_LEN + 1> m_name{};
    std::array<char, detail::T_PLOT_NAME_LEN + 1> m_drop_plot_name{};
    std::array<char, detail::T_PLOT_NAME_LEN + 1> m_enqueue_plot_name{};

    void init_arena() noexcept {
        for (size_t i = 0; i < CAPACITY; i++) {
            this->m_msg_arena[i].set_sequence(i);
        }
    }

    /**
     * Gets a Message from the arena using the given index.
     *
     * @param idx arena index
     * @return mutable reference to the located Message
     */
    Message& get_message(const size_t idx) noexcept { return this->m_msg_arena[idx & MASK]; }

    /**
     * Increments the number of dropped sends and reports the new value to Tracy
     */
    void log_dropped_send() noexcept {
        [[maybe_unused]]
        const auto drop_count =
            static_cast<int64_t>(this->m_dropped.fetch_add(1, std::memory_order_relaxed) + 1);
        if (this->m_report_enabled) {
            TracyPlot(this->m_drop_plot_name.data(), drop_count);
        }
    }

    /**
     *
     */
    void report_queue_length() const noexcept;

protected:
    /**
     * Create a new Channel instance
     */
    Channel() noexcept : m_report_enabled(false) { this->init_arena(); };

    /**
     * Create a new Channel instance
     *
     * @param name the string used to describe this Channel in Tracy
     * @param enable_reporting set to `false` to disable plotting channel stats to Tracy
     */
    explicit Channel(const std::string_view& name, const bool enable_reporting = true) noexcept
        : m_report_enabled(enable_reporting) {
        this->init_arena();

        // Populate the 'm_name' field
        const auto name_len = std::min(name.size(), this->m_name.size() - 1);
        strncpy(this->m_name.data(), name.data(), name_len);

        // Populate the plot names
        strncpy(this->m_drop_plot_name.data(), this->m_name.data(), name_len);
        strncpy(this->m_enqueue_plot_name.data(), this->m_name.data(), name_len);

        const auto cpy_len = detail::T_PLOT_NAME_LEN - name_len;
        strncpy(this->m_drop_plot_name.data() + name_len, ": Dropped", cpy_len);
        strncpy(this->m_enqueue_plot_name.data() + name_len, ": Enqueued", cpy_len);

        // Init plots
        if (this->m_report_enabled) {
            const auto info_msg = std::format("{}: Created", this->m_name.data());
            TracyMessage(info_msg.data(), info_msg.size());

            TracyPlotConfig(
                this->m_drop_plot_name.data(), tracy::PlotFormatType::Number, false, true, 0);
            TracyPlotConfig(
                this->m_enqueue_plot_name.data(), tracy::PlotFormatType::Number, false, true, 0);
        }
    }

    [[nodiscard]] size_t get_dropped() const noexcept {
        return this->m_dropped.load(std::memory_order_relaxed);
    }

    /**
     * Configured capacity of this Channel
     *
     * @return this Channel's capacity
     */
    [[nodiscard]] static consteval size_t capacity() { return CAPACITY; }

    /**
     * Current estimated length of queued messages
     *
     * \remarks The value is estimated due to this method using relaxed atomic loads for
     * performance.
     *
     * @return estimated length of queued messages
     */
    [[nodiscard]] size_t est_length() const noexcept {
        const auto est_len = this->m_tail.load(std::memory_order_relaxed) -
                             this->m_head.load(std::memory_order_relaxed);
        // We might get overflow if the loads are very out of date. It's best to assume the queue is
        // full in this case.
        return est_len < CAPACITY ? est_len : CAPACITY;
    }

    /**
     * Send a message via this Channel
     *
     * \remarks Sends will silently fail without blocking if the Channel is closed.
     *
     * @param payload message data payload
     * @param await_recv set to `true` to block here until a receiver has picked up the message
     */
    void send(T payload, const bool await_recv = false) noexcept {
        if (this->m_state.load(std::memory_order_seq_cst) != detail::ChannelState::Open) {
            // Channel is either in the "DrainOnly" or "Closed" state
            this->log_dropped_send();
            return;
        }

        size_t est_pos = 0;
        auto locate_candidate = [this, &est_pos]() -> bool {
            for (;;) {
                est_pos = this->m_tail.load(std::memory_order_relaxed);

                const auto seq = this->get_message(est_pos).get_sequence();
                if (const auto diff = static_cast<std::ptrdiff_t>(seq - est_pos); diff == 0) {
                    return true;
                } else if (diff > 0) {
                    // est_pos was stale. Go around again
                    continue;
                }

                return false;
            }
        };

        if (!locate_candidate()) {
            // No free slots. Report and return
            this->log_dropped_send();
            return;
        }

        // Spin until we can write the payload to the message
        while (!this->m_tail.compare_exchange_weak(
            est_pos, est_pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            if (!locate_candidate()) {
                // No free slots. Report and return
                this->log_dropped_send();
                return;
            }
        }

        auto& msg = this->get_message(est_pos);

        // Message is now free to write to
        if (await_recv)
            this->m_wait_count.fetch_add(1, std::memory_order_release);
        msg.set_payload(std::move(payload));

        if (this->m_report_enabled)
            this->report_queue_length();

        // Return now if not waiting.
        if (!await_recv)
            return;

        // Await receipt of the message
        msg.await_recv(est_pos + 1);
        this->m_wait_count.fetch_sub(1, std::memory_order_release);
        this->m_wait_count.notify_all();
    }

    /**
     * Tries to receive a pending message from the Channel
     *
     * \remarks this method will immediately return with a `nullopt` if this Channel is closed.
     *
     * @return an optional containing a message payload if one was pending; otherwise, `nullopt`
     */
    std::optional<T> recv() noexcept {
        if (this->m_state.load(std::memory_order_seq_cst) == detail::ChannelState::Closed) {
            // The channel has no living Senders and no queued messages
            return {std::nullopt};
        }

        size_t est_pos = 0;
        auto locate_candidate = [this, &est_pos]() -> bool {
            for (;;) {
                est_pos = this->m_head.load(std::memory_order_relaxed);
                const auto expected_seq = est_pos + 1;
                const auto seq = this->get_message(est_pos).get_sequence();
                if (const auto diff = static_cast<std::ptrdiff_t>(seq - expected_seq); diff == 0) {
                    return true;
                } else if (diff > 0) {
                    // est_pos was stale. Go around again
                    continue;
                }

                return false;
            }
        };

        auto close_channel_check = [this]() noexcept {
            if (this->m_state.load(std::memory_order_seq_cst) == detail::ChannelState::DrainOnly) {
                // Close the channel now since we know we've drained the message queue
                this->m_state.store(detail::ChannelState::Closed, std::memory_order_seq_cst);
            }
        };

        if (!locate_candidate()) {
            close_channel_check();
            return {std::nullopt};
        }

        // Spin until we can read from the cell
        while (!this->m_head.compare_exchange_weak(
            est_pos, est_pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            if (!locate_candidate()) {
                close_channel_check();
                return {std::nullopt};
            }
        }

        return this->get_message(est_pos).consume();
    }

    void inc_sender_count() noexcept {
        this->m_sender_count.fetch_add(1, std::memory_order_seq_cst);
    }

    void dec_sender_count() noexcept {
        const auto send_count = (this->m_sender_count.fetch_sub(1, std::memory_order_seq_cst) - 1);

        auto state = this->m_state.load(std::memory_order_seq_cst);
        if (send_count == 0 && state == detail::ChannelState::Open) {
            // All Senders have hung up or gone out of scope, but Channel is still open.
            const auto did_set_state = this->m_state.compare_exchange_strong(state, detail::ChannelState::DrainOnly, std::memory_order_seq_cst);
            if (did_set_state && this->m_report_enabled) {
                const auto info_msg =
                    std::format("{}: All senders disconnected. State changed to 'DrainOnly'.",
                        this->m_name.data());
                TracyMessage(info_msg.data(), info_msg.size());
            }
        }
    }

    void inc_receiver_count() noexcept {
        this->m_receiver_count.fetch_add(1, std::memory_order_seq_cst);
    }

    void drain_messages() noexcept {
        for (auto& msg : this->m_msg_arena) {
            (void)msg.consume();
        }

        for (size_t num_waiting = this->m_wait_count.load(std::memory_order_acquire);
            num_waiting > 0;)
        {
            this->m_wait_count.wait(num_waiting);
            num_waiting = this->m_wait_count.load(std::memory_order_acquire);
        }
    }

    void dec_receiver_count() noexcept {
        const auto recv_count =
            (this->m_receiver_count.fetch_sub(1, std::memory_order_seq_cst) - 1);

        auto state = this->m_state.load(std::memory_order_seq_cst);
        if (recv_count == 0 && state == detail::ChannelState::Open) {
            // All receivers have hung up or gone out of scope, but Channel is still open
            if (this->m_state.compare_exchange_strong(
                    state, detail::ChannelState::Closed, std::memory_order_seq_cst)) {
                if (this->m_report_enabled) {
                    const auto info_msg =
                    std::format("{}: All receivers disconnected. State changed to 'Closed'",
                        this->m_name.data());
                    TracyMessage(info_msg.data(), info_msg.size());
                }

                // Messages must be drained to ensure waiting senders unblock.
                this->drain_messages();
            }
        }
    }

public:
    /**
     *
     */
    ~Channel() noexcept {
        // Probably not needed, but its good housekeeping.
        this->m_state.store(detail::ChannelState::Closed, std::memory_order_seq_cst);

        // Doing "check then act" here is safe since we have a structural guarantee that noone is holding a handle to
        // the Channel at this point
        if (this->m_wait_count.load(std::memory_order_acquire) > 0) {
            // We only *really* need to drain the messages if any senders are waiting for them to be received.
            this->drain_messages();
        }


        if (this->m_report_enabled) {
            const auto info_msg = std::format("{}: Destroyed", this->m_name.data());
            TracyMessage(info_msg.data(), info_msg.size());
        }
    }

    // Delete all of the copy and move constructors and operators
    Channel(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel& operator=(Channel&&) = delete;

    class ChannelHandle {
    protected:
        std::shared_ptr<Channel> m_channel_ptr;
        ~ChannelHandle() = default;
    public:
        explicit ChannelHandle(std::shared_ptr<Channel> channel_ptr) noexcept : m_channel_ptr(std::move(channel_ptr)) {}
        ChannelHandle() = delete;
        ChannelHandle(const ChannelHandle&) = delete;
        ChannelHandle(ChannelHandle&&) = default;

        [[nodiscard]] size_t get_dropped() const noexcept {
            return this->m_channel_ptr->get_dropped();
        }

        [[nodiscard]] size_t est_length() const noexcept {
            return this->m_channel_ptr->est_length();
        }

        [[nodiscard]] static consteval size_t capacity() noexcept {
            return CAPACITY;
        }
    };

    /**
     * \brief The send side of a Channel
     */
    class Sender : public ChannelHandle {
    public:
        Sender() = delete;
        Sender(Sender&&) = default;

        /*
         * Since constructing, copying, and destructing Senders should never be in hot code paths,
         * it's fine to exchange a little perf for consistent sequencing.
         */

        Sender(const Sender& other) noexcept : ChannelHandle(other.m_channel_ptr) {
            this->m_channel_ptr->inc_sender_count();
        }

        explicit Sender(std::shared_ptr<Channel> channel_ptr) noexcept
            : ChannelHandle(std::move(channel_ptr)) {
            this->m_channel_ptr->inc_sender_count();
        }

        ~Sender() noexcept {
            if (this->m_channel_ptr.get() != nullptr) {
                this->m_channel_ptr->dec_sender_count();
            }
        }

        /**
         * Send a message through the attached Channel
         * @param message payload for the sent message
         */
        void send(T message) noexcept { this->m_channel_ptr->send(std::move(message)); }

        /**
         * Send a message through the attached Channel, blocking until a receiver has picked it up.
         * @param message payload for the sent message
         */
        void send_and_wait(T message) noexcept { this->m_channel_ptr->send(std::move(message), true); }
    };

    /**
     * \brief The receiving side of a Channel
     */
    class Receiver : public ChannelHandle {
    public:
        Receiver() = delete;
        Receiver(const Receiver&) = delete;
        Receiver(Receiver&&) = default;

        explicit Receiver(std::shared_ptr<Channel> channel_ptr) noexcept
            : ChannelHandle(std::move(channel_ptr)) {
            this->m_channel_ptr->inc_receiver_count();
        }

        ~Receiver() noexcept {
            if (this->m_channel_ptr.get() != nullptr) {
                this->m_channel_ptr->dec_receiver_count();
            }
        };

        /**
         * Try to receive a message through the attached Channel.
         * @return an `optional` containing a message payload if one was queued in the Channel.
         */
        std::optional<T> recv() noexcept { return this->m_channel_ptr->recv(); }
    };

    /**
     * Create a new, named Channel and return its Sender/Receiver pair
     * @param name string used in debug reporting for this Channel
     * @param enable_reporting set to `false` to disable reporting Channel stats and events to Tracy
     * @return a Sender/Receiver pair acting as handles to the input and output sides of the
     * Channel, respectively
     */
    static std::pair<Sender, Receiver> make_channel(
        const std::string_view& name, const bool enable_reporting = true) noexcept {
        const auto channel_ptr = std::shared_ptr<Channel>(new Channel(name, enable_reporting));

        auto send = Sender(channel_ptr);
        auto receiver = Receiver(channel_ptr);
        return {std::move(send), std::move(receiver)};
    }
};

template <size_t CAPACITY, std::movable T>
void Channel<CAPACITY, T>::report_queue_length() const noexcept {
    [[maybe_unused]]
    const auto len = static_cast<int64_t>(this->est_length());
    TracyPlot(this->m_enqueue_plot_name.data(), len);
}
}  // namespace dusk