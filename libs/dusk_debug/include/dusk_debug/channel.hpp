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
}  // namespace detail

/**
 * \brief Bounded capacity multi-producer, multi-consumer (MPMC) channel.
 *
 * \remark Users SHOULD call `close()` on the channel when done with it; otherwise, it's possible
 * for the Channel to enter a UB state if `send()` is called on another thread during destruction.
 *
 * \remark Another option is to wrap the channel in a `std::shared_ptr`. Each thread gets its own
 * copy, and the destructor should only fire once the Channel is guaranteed to be out of scope
 * across all threads.
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

    /// Count of in-flight sends/receives
    /// \remark should only be active in debug builds
    alignas(std::hardware_destructive_interference_size) std::atomic_size_t m_in_flight{0};
    /// Flag indicating if the channel has been closed and cannot accept new messages.
    std::atomic_bool m_closed{false};

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

#ifndef NDEBUG
    /**
     * RAII wrapper used to aid in tracking the current number of in-flight channel actions.
     */
    class FlightRecorder {
        std::atomic_size_t& m_in_flight_ref;
    public:
        /**
         * Create a new FlightRecorder, incrementing the Channel's in-flight count.
         *
         * \param channel channel instance to use
         */
        explicit FlightRecorder(Channel& channel) noexcept : m_in_flight_ref(channel.m_in_flight) {
            this->m_in_flight_ref.fetch_add(1, std::memory_order_acquire);
        }

        FlightRecorder(FlightRecorder const&) = delete;
        FlightRecorder(FlightRecorder&&) = delete;

        /**
         * Destroy this FlightRecorder, decrementing the Channel's in-flight count.
         */
        ~FlightRecorder() {
            this->m_in_flight_ref.fetch_sub(1, std::memory_order_release);
            this->m_in_flight_ref.notify_all();
        }
    };
#else
    class FlightRecorder {
    public:
        explicit FlightRecorder([[maybe_unused]] Channel& channel) noexcept {}

        FlightRecorder(FlightRecorder const&) = delete;
        FlightRecorder(FlightRecorder&&) = delete;
    };
#endif
public:
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

    /**
     * Closes this Channel, dropping all future sends and pending messages.
     */
    void close() noexcept {
        if (this->m_closed.load(std::memory_order_seq_cst)) {
            // Return early if called on a closed Channel. We have to assume that the caller has
            // cleaned up after themselves.
            return;
        }
        this->m_closed.store(true, std::memory_order_seq_cst);

        for (auto& msg : this->m_msg_arena) {
            (void)msg.consume();
        }

        for (size_t num_waiting = this->m_wait_count.load(std::memory_order_acquire);
            num_waiting > 0;)
        {
            this->m_wait_count.wait(num_waiting);
            num_waiting = this->m_wait_count.load(std::memory_order_acquire);
        }

        for (size_t num_waiting = this->m_in_flight.load(std::memory_order_seq_cst);
            num_waiting > 0;)
        {
            this->m_in_flight.wait(num_waiting);
            num_waiting = this->m_in_flight.load(std::memory_order_seq_cst);
        }

        if (this->m_report_enabled) {
            const auto info_msg = std::format("{}: Closed", this->m_name.data());
            TracyMessage(info_msg.data(), info_msg.size());
        }
    }

    /**
     *
     */
    ~Channel() {
        this->close();

        if (this->m_report_enabled) {
            const auto info_msg = std::format("{}: Destroyed", this->m_name.data());
            TracyMessage(info_msg.data(), info_msg.size());
        }
    }

    [[nodiscard]] size_t get_dropped() const noexcept {
        return this->m_dropped.load(std::memory_order_relaxed);
    }

    // Delete all of the copy and move constructors and operators

    Channel(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel& operator=(Channel&&) = delete;

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
        FlightRecorder flight_rec{*this};
        if (this->m_closed.load(std::memory_order_seq_cst)) {
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
            this->m_wait_count.fetch_add(1, std::memory_order_relaxed);
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
        FlightRecorder flight_rec{*this};
        if (this->m_closed.load(std::memory_order_seq_cst)) {
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

        if (!locate_candidate()) {
            return {std::nullopt};
        }

        // Spin until we can read from the cell
        while (!this->m_head.compare_exchange_weak(
            est_pos, est_pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            if (!locate_candidate()) {
                return {std::nullopt};
            }
        }

        return this->get_message(est_pos).consume();
    }
};

template <size_t CAPACITY, std::movable T>
void Channel<CAPACITY, T>::report_queue_length() const noexcept {
    [[maybe_unused]]
    const auto len = static_cast<int64_t>(this->est_length());
    TracyPlot(this->m_enqueue_plot_name.data(), len);
}

extern template class dusk::Channel<16, int32_t>;
}  // namespace dusk