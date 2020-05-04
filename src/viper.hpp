#pragma once

#include <iostream>
#include <tbb/concurrent_hash_map.h>
#include <libpmemobj++/shared_mutex.hpp>
#include <bitset>
#include <libpmem.h>
#include <libpmemobj++/make_persistent_atomic.hpp>
#include <libpmemobj++/pool.hpp>
#include <libpmemobj++/container/vector.hpp>
#include <filesystem>
#include <thread>
#include <cmath>

namespace viper {

namespace pobj = pmem::obj;

using offset_size_t = uint64_t;
using block_size_t = uint64_t;
using page_size_t = uint8_t;
using slot_size_t = uint8_t;
using version_lock_size_t = uint64_t;

static constexpr uint16_t PAGE_SIZE = 4 * 1024; // 4kb
static constexpr uint16_t MIN_PAGE_SIZE = PAGE_SIZE / 4; // 1kb
static constexpr uint16_t MAX_PAGE_SIZE = PAGE_SIZE * 6; // 1kb
static constexpr uint8_t NUM_DIMMS = 6;
static constexpr double RESIZE_THRESHOLD = 0.75;
static constexpr block_size_t NUM_BLOCKS_PER_CREATE = 10;
static constexpr slot_size_t BASE_NUM_SLOTS_PER_PAGE = 64;

static constexpr version_lock_size_t LOCK_BIT = 0x8000000000000000;
static constexpr version_lock_size_t COUNTER_MASK = ~LOCK_BIT;

static constexpr block_size_t NULL_BLOCK = std::numeric_limits<block_size_t>::max();

namespace internal {

template <typename K, typename V>
constexpr slot_size_t get_num_slots_per_page() {
//    const uint32_t entry_size = sizeof(K) + sizeof(V);
//    uint16_t current_page_size = MIN_PAGE_SIZE;
//    slot_size_t num_slots_per_page = 64;
//    const uint16_t page_overhead = sizeof(version_lock_size_t) + sizeof(std::bitset<BASE_NUM_SLOTS_PER_PAGE>);
//
//    while ((entry_size * num_slots_per_page) - 16 > current_page_size) {
//        current_page_size *= 2;
//    }
//    assert(current_page_size <= MAX_PAGE_SIZE && "Cannot fit 64 KV pairs into single page!");
//
//    while ((num_slots_per_page * entry_size) + page_overhead + std::ceil(num_slots_per_page / 8) > PAGE_SIZE) {
//        num_slots_per_page--;
//    }
//    assert(num_slots_per_page > 0 && "Cannot fit KV pair into single page!");
    // Hard code for now based on 8 byte key + 8 byte value
    return 253;
}
class VersionLock {
  public:


};

template <typename K, typename V>
struct alignas(MIN_PAGE_SIZE) ViperPage {
    using VEntry = std::pair<K, V>;
    static constexpr slot_size_t num_slots_per_page = get_num_slots_per_page<K, V>();

    std::atomic<version_lock_size_t> version_lock;
    std::bitset<num_slots_per_page> free_slots;
    std::array<VEntry, num_slots_per_page> data;
    uint64_t padding_;

    ViperPage() {
        static_assert(sizeof(*this) >= 1024, "VPage needs to be at least 1024 byte!");
        static_assert(PAGE_SIZE % sizeof(*this) == 0, "VPage not page size conform!");
        version_lock = 0;
        free_slots.flip();
        assert(free_slots.all());
    }
};

template <typename VPage, page_size_t num_pages>
struct alignas(PAGE_SIZE) ViperPageBlock {
    static constexpr uint64_t num_slots_per_block = VPage::num_slots_per_page * num_pages;
    /**
     * Array to store all persistent ViperPages.
     * Don't use a vector here because a ViperPage uses arrays and the whole struct would be moved on a vector resize,
     * making all pointers invalid.
     */
    std::array<VPage, num_pages> v_pages;
};

class KeyValueOffset {
  public:
    KeyValueOffset() : offset{0xFFFFFFFFFFFFFFFF} {}

    KeyValueOffset(const block_size_t block_number, const page_size_t page_number, const slot_size_t slot)
        : offset{shift_numbers(block_number, page_number, slot)} {}

    inline std::tuple<block_size_t, page_size_t, slot_size_t> get_offsets() const {
        return {get_block_number(), get_page_number(), get_slot_number()};
    }

    inline block_size_t get_block_number() const {
        return (offset & 0xFFFFFFFFFFFF0000u) >> 16u; // Bit 1 to 48
    }

    inline page_size_t get_page_number() const {
        return (offset & 0xFF00u) >> 8u; // Bits 49 to 56
    }

    inline slot_size_t get_slot_number() const {
        return offset & 0xFFu; // Bits 57 to 64
    }

  protected:
    static offset_size_t shift_numbers(const block_size_t block_number, const page_size_t page_number, const slot_size_t slot) {
        offset_size_t kv_offset = block_number << 16u;
        kv_offset |= static_cast<offset_size_t>(page_number) << 8u;
        kv_offset |= static_cast<offset_size_t>(slot);
        return kv_offset;
    }

    offset_size_t offset;
};

} // namespace internal

template <typename K, typename V>
struct ViperRoot {
    using VPage = internal::ViperPage<K, V>;
    static constexpr uint64_t v_page_size = sizeof(VPage);
    static constexpr page_size_t num_pages_per_block = NUM_DIMMS * (PAGE_SIZE / v_page_size);
    using VPageBlock = internal::ViperPageBlock<VPage, num_pages_per_block>;
    using VPageBlocks = pobj::vector<pobj::persistent_ptr<VPageBlock>>;

    VPageBlocks v_page_blocks;

    VPageBlock* create_new_block() {
        pobj::persistent_ptr<VPageBlock> new_block = pobj::make_persistent<VPageBlock>();
        v_page_blocks.push_back(new_block);
        return new_block.get();
    }
};

template <typename K, typename V>
class Viper {
    using KVOffset = internal::KeyValueOffset;
    using MapType = tbb::concurrent_hash_map<K, KVOffset>;
    using VPage = internal::ViperPage<K, V>;
    using VRoot = ViperRoot<K, V>;
    using VPageBlock = typename VRoot::VPageBlock;
    using VPageBlocks = typename VRoot::VPageBlocks;

  public:
    Viper(const std::string& pool_file, uint64_t pool_size);
    explicit Viper(pobj::pool<ViperRoot<K, V>>&& v_pool);
    explicit Viper(const pobj::pool<ViperRoot<K, V>>& v_pool);
    ~Viper();

    bool put(K key, V value);
    V get(K key);
    bool remove(K key);
    size_t count();

  protected:
    Viper(const pobj::pool<ViperRoot<K, V>>& v_pool, bool owns_pool);
    pobj::pool<VRoot> init_pool(const std::string& pool_file, uint64_t pool_size);

    inline block_size_t get_next_block();
    inline VPage* get_v_page(block_size_t block_number, page_size_t page_number);
    void add_v_page_blocks(block_size_t num_blocks = 1);

    pobj::pool<VRoot> v_pool_;
    pobj::persistent_ptr<VRoot> v_root_;
    MapType map_;

    const bool owns_pool_;
    std::vector<VPageBlock*> blocks_;
    // Store counter for each page. This does not need to be atomic because only one thread can access a page at a time.
    std::vector<std::array<slot_size_t, VRoot::num_pages_per_block>> block_remaining_slots_;

    const uint16_t num_slots_per_block_;
    const page_size_t num_pages_per_block_;
    const uint16_t capacity_per_block_;
    std::atomic<uint64_t> current_block_;
    uint64_t current_page_;

    std::atomic<size_t> current_size_;
    size_t current_capacity_;
    std::atomic<uint16_t> current_block_capacity_;
    size_t resize_at_;
    const double resize_threshold_;
    std::atomic<bool> is_resizing_;
    std::unique_ptr<std::thread> resize_thread_;

    std::atomic<bool> is_concurrent_;
};

template <typename K, typename V>
Viper<K, V>::Viper(const std::string& pool_file, const uint64_t pool_size) : Viper{init_pool(pool_file, pool_size)} {}

template <typename K, typename V>
Viper<K, V>::Viper(pobj::pool<ViperRoot<K, V>>&& v_pool) : Viper{v_pool, true} {}

template <typename K, typename V>
Viper<K, V>::Viper(const pobj::pool<ViperRoot<K, V>>& v_pool) : Viper{v_pool, false} {}

template <typename K, typename V>
Viper<K, V>::Viper(const pobj::pool<ViperRoot<K, V>>& v_pool, bool owns_pool) :
    v_pool_{v_pool}, v_root_{v_pool_.root()}, map_{VRoot::VPageBlock::num_slots_per_block}, owns_pool_{owns_pool},
    num_slots_per_block_{VRoot::VPageBlock::num_slots_per_block}, num_pages_per_block_{VRoot::num_pages_per_block},
    capacity_per_block_{static_cast<uint16_t>(num_slots_per_block_ - num_pages_per_block_)}, current_block_{0},
    current_page_{0}, current_size_{0}, current_capacity_{0}, current_block_capacity_{capacity_per_block_},
    resize_threshold_{RESIZE_THRESHOLD}, is_resizing_{false} {
    // TODO: build map here and stuff
    add_v_page_blocks(NUM_BLOCKS_PER_CREATE);
}

template <typename K, typename V>
Viper<K, V>::~Viper() {
    if (owns_pool_) {
        std::cout << "Closing pool file." << std::endl;
        v_pool_.close();
    }
}

template <typename K, typename V>
bool Viper<K, V>::put(K key, V value) {
    block_size_t block_number;
    page_size_t v_page_number;
    VPage* v_page;
    version_lock_size_t lock_value;
    std::bitset<VPage::num_slots_per_page>* free_slots;
    std::bitset<VPage::num_slots_per_page> free_slot_checker;
    slot_size_t free_slot_idx;

    // Find free slot
    do {
        block_number = get_next_block();
        v_page_number = ++current_page_ % num_pages_per_block_;
        v_page = get_v_page(block_number, v_page_number);

        // Lock v_page
        std::atomic<version_lock_size_t>& v_lock = v_page->version_lock;
        // We expect the lock bit to be unset
        lock_value = v_lock.load(std::memory_order_acquire) & ~LOCK_BIT;
        // Compare and swap until we are the thread to set the lock bit
        while (!v_lock.compare_exchange_weak(lock_value, lock_value | LOCK_BIT)) {
            lock_value &= ~LOCK_BIT;
        }

        // We now have the lock on this page
        free_slots = &(v_page->free_slots);
        free_slot_idx = free_slots->_Find_first();
        free_slot_checker = *free_slots;

        // Always keep one slot free for updates
        if (free_slot_checker.reset(free_slot_idx).none()) {
            // Free lock on page and restart
            v_lock.store(lock_value & ~LOCK_BIT, std::memory_order_release);
            continue;
        }

        // We have found a free slot on this page
        break;
    } while (true);

//    --(block_remaining_slots_[block_number][v_page_number]);
    --current_block_capacity_;

    v_page->data[free_slot_idx] = {key, value};
    const typename VPage::VEntry* entry_ptr = v_page->data.data() + free_slot_idx;
    pmemobj_persist(v_pool_.handle(), entry_ptr, sizeof(typename VPage::VEntry));
    free_slots->flip(free_slot_idx);
    pmemobj_persist(v_pool_.handle(), free_slots, sizeof(free_slots));

    const KVOffset kv_offset{block_number, v_page_number, free_slot_idx};
    bool is_new_item;
    {
        // Scope this so the accessor is free'd as soon as possible.
        typename MapType::accessor accessor;
        is_new_item = map_.insert(accessor, {key, kv_offset});
        if (!is_new_item) {
            accessor->second = kv_offset;
        }
    }

    // Unlock the v_page and increment the version counter
    // Bump version number and unset lock bit
    std::atomic<version_lock_size_t>& v_lock = v_page->version_lock;
    version_lock_size_t old_version_number = lock_value & COUNTER_MASK;
    version_lock_size_t new_version_lock = (old_version_number + 1) & ~LOCK_BIT;
    v_lock.store(new_version_lock, std::memory_order_release);

    if (++current_size_ < resize_at_) {
        // Enough capacity, no need to resize.
        return is_new_item;
    }

    bool expected_resizing = false;
    const bool should_resize = is_resizing_.compare_exchange_strong(expected_resizing, true);
    if (should_resize) {
        // Only one thread can ever get here because for all others the atomic exchange above fails.
        resize_thread_ = std::make_unique<std::thread>([this] {
            this->add_v_page_blocks(NUM_BLOCKS_PER_CREATE);
        });
        resize_thread_->detach();
    }

    return is_new_item;
}

template <typename K, typename V>
V Viper<K, V>::get(K key) {
    typename MapType::const_accessor result;
    const bool found = map_.find(result, key);
    if (!found) {
        throw std::runtime_error("Key '" + std::to_string(key) + "' not found.");
    }

    const KVOffset kv_offset = result->second;
    const auto [block_number, page_number, slot_number] = kv_offset.get_offsets();
    // TODO: check how this is optimized by compiler
    return blocks_[block_number]->v_pages[page_number].data[slot_number].second;
}

template <typename K, typename V>
size_t Viper<K, V>::count() {
    return current_size_;
}

template <typename K, typename V>
block_size_t Viper<K, V>::get_next_block() {
    if (current_block_capacity_.load() == 0) {
        // No more capacity in current block
        {
            uint16_t expected_capacity = 0;
            const bool swap_successful = current_block_capacity_.compare_exchange_strong(expected_capacity, capacity_per_block_);
            if (swap_successful) {
                ++current_block_;
            }
        }
    }

    return current_block_;
}

template <typename K, typename V>
internal::ViperPage<K, V>* Viper<K, V>::get_v_page(const block_size_t block_number, const page_size_t page_number) {
    // TODO: check how this is optimized by compiler
    VPageBlock* next_v_block = blocks_[block_number];
    return &(next_v_block->v_pages[page_number]);
}

template <typename K, typename V>
pobj::pool<ViperRoot<K, V>> Viper<K, V>::init_pool(const std::string& pool_file, const uint64_t pool_size) {
    int sds_write_value = 0;
    pmemobj_ctl_set(NULL, "sds.at_create", &sds_write_value);

    if (std::filesystem::exists(pool_file)) {
        std::cout << "Opening pool file " << pool_file << std::endl;
        return pmem::obj::pool<VRoot>::open(pool_file, "");
    } else {
        std::cout << "Creating pool file " << pool_file << std::endl;
        pobj::pool<VRoot> v_pool = pmem::obj::pool<VRoot>::create(pool_file, "", pool_size, S_IRWXU);
        pobj::transaction::run(v_pool, [&] {
            v_pool.root()->create_new_block();
        });
        return v_pool;
    }
}

template <typename K, typename V>
void Viper<K, V>::add_v_page_blocks(const block_size_t num_blocks) {
    const block_size_t num_blocks_before = blocks_.size();
    current_capacity_ += num_slots_per_block_ * num_blocks;
    resize_at_ = current_capacity_ * resize_threshold_;

//    std::cout << "Adding block (size: " << blocks_.size() << ")" << std::endl;
    // Keep 1 slot per page free to allow for updates in same page;
    const slot_size_t page_capacity = VPage::num_slots_per_page - 1;

    const block_size_t num_total_blocks = num_blocks_before + num_blocks;
    for (block_size_t block_id = num_blocks_before; block_id < num_total_blocks; ++block_id) {
        pobj::transaction::run(v_pool_, [&] {
            VPageBlock* new_block = v_pool_.root()->create_new_block();
            blocks_.push_back(new_block);
        });
        block_remaining_slots_.emplace_back();
        block_remaining_slots_.back().fill(page_capacity);
    }

    is_resizing_ = false;
//    std::cout << "Block added (size: " << blocks_.size() << ")" << std::endl;
}

}  // namespace viper