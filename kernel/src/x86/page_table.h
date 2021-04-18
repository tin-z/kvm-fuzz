#ifndef _X86_PAGE_TABLE_H
#define _X86_PAGE_TABLE_H

#include "common.h"

// Page table stuff
#define PTL4_SHIFT 39
#define PTL4_BITS   9
#define PTL4_SIZE  (1UL << PTL4_SHIFT)
#define PTL4_MASK  (~(PTL4_SIZE - 1))
#define PTRS_PER_PTL4 (1UL << PTL4_BITS)
#define PTL4_INDEX(addr) ((addr >> PTL4_SHIFT) & (PTRS_PER_PTL4 - 1))

#define PTL3_SHIFT 30
#define PTL3_BITS   9
#define PTL3_SIZE  (1UL << PTL3_SHIFT)
#define PTL3_MASK  (~(PTL3_SIZE - 1))
#define PTRS_PER_PTL3 (1UL << PTL3_BITS)
#define PTL3_INDEX(addr) ((addr >> PTL3_SHIFT) & (PTRS_PER_PTL3 - 1))

#define PTL2_SHIFT 21
#define PTL2_BITS   9
#define PTL2_SIZE  (1UL << PTL2_SHIFT)
#define PTL2_MASK  (~(PTL2_SIZE - 1))
#define PTRS_PER_PTL2 (1UL << PTL2_BITS)
#define PTL2_INDEX(addr) ((addr >> PTL2_SHIFT) & (PTRS_PER_PTL2 - 1))

#define PTL1_SHIFT 12
#define PTL1_BITS  9
#define PTL1_SIZE  (1UL << PTL1_SHIFT)
#define PTL1_MASK  (~(PTL1_SIZE - 1))
#define PTRS_PER_PTL1 (1UL << PTL1_BITS)
#define PTL1_INDEX(addr) ((addr >> PTL1_SHIFT) & (PTRS_PER_PTL1 - 1))

#define PAGE_SIZE PTL1_SIZE
#define PAGE_OFFSET(addr) ((addr) & (~PTL1_MASK))
#define PAGE_CEIL(addr) (((addr) + PAGE_SIZE - 1) & PTL1_MASK) //+ 0xFFF & ~0xFFF
#define IS_PAGE_ALIGNED(addr) (((addr) & PTL1_MASK) == (addr))

#define PHYS_MASK (0x000FFFFFFFFFF000)
#define PHYS_FLAGS(addr) ((addr) & (~PHYS_MASK))

class PageTableEntry {
public:
	enum Flags {
		Present   = (1 << 0),
		ReadWrite = (1 << 1),
		User      = (1 << 2),
		Accessed  = (1 << 5),
		Dirty     = (1 << 6),
		Huge      = (1 << 7),
		Global    = (1 << 8),
		NoExecute = (1UL << 63)
	};

	uintptr_t raw() {
		return m_raw;
	}

	uintptr_t frame_base() {
		return m_raw & PHYS_MASK;
	}

	void set_frame_base(uintptr_t base) {
		ASSERT((base & PHYS_MASK) == base, "invalid base: %p", base);
		m_raw &= ~PHYS_MASK;
		m_raw |= base;
	}

	void set_flags(uint64_t page_flags) {
		ASSERT((page_flags & ~PHYS_MASK) == page_flags, "invalig page flags: %p",
		       page_flags);
		m_raw &= PHYS_MASK;
		m_raw |= page_flags;
	}

	void clear() { m_raw = 0; }

	bool is_present() const { return m_raw & Flags::Present; }
	void set_present(bool b) { set_bit(Flags::Present, b); }

	bool is_writable() const { return m_raw & Flags::ReadWrite; }
	void set_writable(bool b) { set_bit(Flags::ReadWrite, b); }

	bool is_user() const { return m_raw & Flags::User; }
	void set_user(bool b) { set_bit(Flags::User, b); }

	bool is_huge() const { return m_raw & Flags::Huge; }
	void set_huge(bool b) { set_bit(Flags::Huge, b); }

	bool is_global() const { return m_raw & Flags::Global; }
	void set_global(bool b) { set_bit(Flags::Global, b); }

	bool is_execute_disabled() const { return m_raw & Flags::NoExecute; }
	void set_execute_disabled(bool b) { set_bit(Flags::NoExecute, b); }

private:

	void set_bit(uint64_t bit, bool value) {
		if (value)
			m_raw |= bit;
		else
			m_raw &= ~bit;
	}

	uintptr_t m_raw;
};

// YOLO
typedef PageTableEntry PageTableLevel2Entry;
typedef PageTableEntry PageTableLevel3Entry;
typedef PageTableEntry PageTableLevel4Entry;

static_assert(sizeof(PageTableEntry) == 8);

#endif