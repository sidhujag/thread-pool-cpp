#pragma once

#include <thread_pool/mpmc_bounded_queue.hpp>

#include <atomic>

namespace tp
{

/**
* @brief The Slotted Bag is a lockless container which tracks membership by id. 
* The bag is divided into slots, each with a unique id, and each slot can be full or empty.
* The bag supports filling and emptying slots, as well as the unordered emptying of any full slot. 
* The bag supports multiple consumers, and a single producer per slot.
*/
class SlottedBag
{
    /**
    * @brief The implementation of a single slot of the bag. Maintains state and id.
    * @details State Machine:
    *
    *             +------ TryRemoveAny -------+-------------------------------+ 
    *             |                           |                               | 
    *             v                           |                               | 
    *       +-----------+              +-------------+                 +---------------+           
    *       | NotQueued | -- add() --> | QueuedValid | -- remove() --> | QueuedInvalid |
    *       +-----------+              +-------------+                 +---------------+
    *                                         ^                               |
    *                                         |                               |
    *                                         +------------- add() -----------+
    */
    struct Slot
    {
        enum class State
        {
            NotQueued,
            QueuedValid,
            QueuedInvalid,
        };

        std::atomic<State> state;
        size_t id;

        Slot()
            : state(State::NotQueued)
            , id(0)
        {
        }

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        Slot(Slot&& rhs) = default;
        Slot& operator=(Slot&& rhs) = default;
    };

public:

    /**
    * @brief SlottedBag Constructor.
    * @param size Power of 2 number - queue length.
    * @throws std::invalid_argument if size is not a power of 2.
    */
    explicit SlottedBag(size_t size);

    SlottedBag(SlottedBag& rhs) = delete;
    SlottedBag& operator=(SlottedBag& rhs) = delete;

    SlottedBag(SlottedBag&& rhs) = default;
    SlottedBag& operator=(SlottedBag&& rhs) = default;

    /**
    * @brief fill Fill the specified slot.
    * @param id The id of the slot to fill.
    * @throws std::runtime_error if the slot is already full.
    * @note Other exceptions may be thrown if the single-producer-per-slot
    * semantics are violated.
    */
    void fill(size_t id);

    /**
    * @brief empty Empties the slot with the specified id.
    * @param id The id of the slot to empty.
    * @returns true if the state of the bag was changed, false otherwise. 
    * @note The slot will be empty after the call regardless of the return value.
    */
    bool empty(size_t id);

    /**
    * @brief tryEmptyAny Try to empty any slot in the bag.
    * @param id The emptied slot id will be stored in this variable upon success.
    * @return true upon success, false otherwise.
    * @note Other exceptions may be thrown if the single-producer-per-slot
    * semantics are violated.
    */
    bool tryEmptyAny(size_t& id);

private:
    MPMCBoundedQueue<Slot*> m_queue;
    std::vector<Slot> m_slots;
};

inline SlottedBag::SlottedBag(size_t size)
    : m_queue(size)
    , m_slots(size)
{
    for (auto i = 0u; i < size; i++)
        m_slots[i].id = i;
}

inline void SlottedBag::fill(size_t id)
{
    switch (m_slots[id].state.exchange(Slot::State::QueuedValid, std::memory_order_acq_rel))
    {
    case Slot::State::NotQueued:
        // No race, single producer per slot.
        while (!m_queue.pushWeak(&m_slots[id])); // Queue should never overflow. Use weak form for performance.
        break;

    case Slot::State::QueuedValid:
        throw std::runtime_error("The item has already been added to the bag.");

    case Slot::State::QueuedInvalid:
        // Item will still be in the queue. We are done.
        break;
    }
}

inline bool SlottedBag::empty(size_t id)
{
    // This consumer action is solely responsible for an indiscriminant QueuedValid -> QueuedInvalid state transition.
    auto state = Slot::State::QueuedValid;
    return m_slots[id].state.compare_exchange_strong(state, Slot::State::QueuedInvalid, std::memory_order_acq_rel);
}

inline bool SlottedBag::tryEmptyAny(size_t& id)
{
    Slot* slot;
    while (m_queue.popStrong(slot))
    {
        // Once a consumer pops a slot, they are the sole controller of that object's membership to the queue
        // (i.e. they are solely responsible for the transition back into the NotQueued state) by virtue of the
        // MPMCBoundedQueue's atomicity semantics.
        // In other words, only one consumer can hold a popped slot before it its state is set to NotQueued.
        switch (slot->state.exchange(Slot::State::NotQueued, std::memory_order_acq_rel))
        {
        case Slot::State::NotQueued:
            throw std::logic_error("State machine logic violation.");

        case Slot::State::QueuedValid:
            id = slot->id;
            return true;

        case Slot::State::QueuedInvalid:
            // Try again.
            break;
        }
    }

    // Queue empty.
    return false;
}

}