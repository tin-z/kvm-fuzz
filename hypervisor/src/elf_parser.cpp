#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <limits>
#include "elf_parser.h"
#include "utils.h"

#define PAGE_CEIL(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

using namespace std;

ElfParser::ElfParser(const string& elf_path)
	: m_path(elf_path)
{
	const char* cpath = m_path.c_str();

	// Load file into memory
	struct stat st;
	int fd = open(cpath, O_RDONLY);
	ERROR_ON(fd < 0, "elf %s: open", cpath);
	ERROR_ON(fstat(fd, &st) < 0, "elf %s: fstat", cpath);
	m_size = st.st_size;

	m_data = (uint8_t*)mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, fd, 0);
	ERROR_ON(m_data == MAP_FAILED, "elf %s: mmap", cpath);
	close(fd);

	init();
}

ElfParser::ElfParser(const string& elf_path, const void* data, vsize_t size)
	: m_path(elf_path)
	, m_data((const uint8_t*)data)
	, m_size(size)
{
	init();
}

void ElfParser::init() {
	const char* cpath = m_path.c_str();

	Elf_Ehdr* ehdr = (Elf_Ehdr*)m_data;
	Elf_Phdr* phdr = (Elf_Phdr*)(m_data + ehdr->e_phoff);
	Elf_Shdr* shdr = (Elf_Shdr*)(m_data + ehdr->e_shoff);
	m_phinfo = {
		.e_phoff     = ehdr->e_phoff,
		.e_phentsize = ehdr->e_phentsize,
		.e_phnum     = ehdr->e_phnum
	};
	m_type  = ehdr->e_type;
	m_entry = ehdr->e_entry;

	// Some checks
	ASSERT(ehdr->e_ident[EI_CLASS] == ELFCLASS,
	       "elf %s: BITS don't match (expecting %d)", cpath, BITS);
	ASSERT(ehdr->e_machine == EM,
	       "elf %s: MACH doesn't match (expecting %s)", cpath, EM_S);
	ASSERT(ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN,
	       "elf %s: TYPE doesn't match (expecting executable or shared", cpath);

	// Get segments
	m_load_addr = numeric_limits<vaddr_t>::max();
	m_initial_brk = 0;
	size_t i;
	for (i = 0; i < ehdr->e_phnum; i++) {
		segment_t segment = {
			.type     = phdr[i].p_type,
			.flags    = phdr[i].p_flags,
			.offset   = phdr[i].p_offset,
			.vaddr    = phdr[i].p_vaddr,
			.paddr    = phdr[i].p_paddr,
			.filesize = phdr[i].p_filesz,
			.memsize  = phdr[i].p_memsz,
			.align    = phdr[i].p_align,
			.data     = m_data + segment.offset
		};
		m_segments.push_back(segment);

		// Update brk beyond any loadable segment, and load_addr as the address
		// of the first segment in memory
		if (segment.type == PT_LOAD) {
			vaddr_t next_page = PAGE_CEIL(segment.vaddr + segment.memsize);
			m_initial_brk = max(m_initial_brk, next_page);
			m_load_addr = min(m_load_addr, segment.vaddr);
		}
		if (segment.type == PT_INTERP)
			m_interpreter = string((char*)segment.data);
	}

	// If this is a PIE binary, make sure its load address is 0 at first
	if (m_type == ET_DYN)
		ASSERT(m_load_addr == 0, "PIE %s binary with load addr %p", cpath, m_load_addr);

	// Get sections
	Elf_Shdr* sh_strtab = &shdr[ehdr->e_shstrndx];
	const char* strtab = (char*)m_data + sh_strtab->sh_offset;
	for (i = 0; i < ehdr->e_shnum; i++) {
		section_t section = {
			.name      = string(strtab + shdr[i].sh_name),
			.type      = shdr[i].sh_type,
			.flags     = shdr[i].sh_flags,
			.addr      = shdr[i].sh_addr,
			.offset    = shdr[i].sh_offset,
			.size      = shdr[i].sh_size,
			.link      = shdr[i].sh_link,
			.info      = shdr[i].sh_info,
			.addralign = shdr[i].sh_addralign,
			.entsize   = shdr[i].sh_entsize,
			.data      = m_data + section.offset
		};
		m_sections.push_back(section);
	}

	// Get symbols
	for (const section_t& section : m_sections) {
		// Symbols are defined in these two sections
		if (section.type != SHT_SYMTAB && section.type != SHT_DYNSYM)
			continue;

		Elf_Sym* syms = (Elf_Sym*)section.data;
		size_t n_syms = section.size / sizeof(Elf_Sym);

		// The string table could be sections .strtab or .dynstr. The index of
		// the string table section is specified in the symbol section link
		const char* sec_strtab = (char*)m_sections[section.link].data;
		for (i = 0; i < n_syms; i++) {
			symbol_t symbol = {
				.name       = string(sec_strtab + syms[i].st_name),
				.type       = (uint8_t)ELF_ST_TYPE(syms[i].st_info),
				.binding    = (uint8_t)ELF_ST_BIND(syms[i].st_info),
				.visibility = (uint8_t)ELF_ST_VISIBILITY(syms[i].st_other),
				.shndx      = syms[i].st_shndx,
				.value      = syms[i].st_value,
				.size       = syms[i].st_size
			};
			m_symbols.push_back(symbol);
		}
	}

	// Get dependencies using ldd
	string ldd_result = exec_cmd("ldd " + m_path + " 2>&1");
	vector<string> lines = split_string(ldd_result, "\n");
	for (const string& line : lines) {
		size_t pos1 = line.find("=> ");
		if (pos1 == string::npos)
			continue;
		pos1 += 3;
		size_t pos2 = line.find(" ", pos1); // assume path doesn't have spaces :')
		if (pos2 == string::npos)
			continue;
		m_dependencies.push_back(line.substr(pos1, pos2-pos1));
	}

	m_debug = ElfDebug(m_data, m_size);
}

const uint8_t* ElfParser::data() const {
	return m_data;
}

vsize_t ElfParser::size() const {
	return m_size;
}

void ElfParser::set_load_addr(vaddr_t load_addr) {
	ASSERT(m_type == ET_DYN, "setting load_addr to not PIE binary %s", m_path.c_str());

	vaddr_t diff = load_addr - m_load_addr;
	m_load_addr = load_addr;

	// Update all virtual addresses accordingly
	m_entry       += diff;
	m_initial_brk += diff;
	for (segment_t& segment : m_segments) {
		segment.vaddr += diff;
		segment.paddr += diff;
	}
	for (section_t& section : m_sections) {
		section.addr += diff;
	}
	for (symbol_t& symbol : m_symbols) {
		symbol.value += diff;
	}
}

vaddr_t ElfParser::load_addr() const {
	return m_load_addr;
}

vaddr_t ElfParser::initial_brk() const {
	return m_initial_brk;
}

phinfo_t ElfParser::phinfo() const {
	return m_phinfo;
}

uint16_t ElfParser::type() const {
	return m_type;
}

vaddr_t ElfParser::entry() const {
	return m_entry;
}

string ElfParser::path() const {
	return m_path;
}

string ElfParser::interpreter() const {
	return m_interpreter;
}

vector<segment_t> ElfParser::segments() const {
	return m_segments;
}

vector<section_t> ElfParser::sections() const {
	return m_sections;
}

vector<symbol_t> ElfParser::symbols() const {
	return m_symbols;
}

vector<string> ElfParser::dependencies() const {
	return m_dependencies;
}

pair<vaddr_t, vaddr_t> ElfParser::section_limits(const string& name) const {
	for (const section_t& section : sections())
		if (section.name == name)
			return { section.addr, section.addr + section.size };
	ASSERT(false, "not found section: %s", name.c_str());
}

pair<vaddr_t, vaddr_t> ElfParser::symbol_limits(const string& name) const {
	for (const symbol_t& symbol : symbols())
		if (symbol.name == name)
			return { symbol.value, symbol.value + symbol.size };
	ASSERT(false, "not found symbol: %s", name.c_str());
}

bool ElfParser::has_dwarf() const {
	return m_debug.has();
}

bool ElfParser::addr_to_symbol(vaddr_t addr, symbol_t& result) const {
	for (const symbol_t& symbol : symbols()) {
		if (addr >= symbol.value && addr < symbol.value + symbol.size) {
			result = symbol;
			return true;
		}
	}
	return false;
}

string ElfParser::addr_to_source(vaddr_t addr) const {
	// Substract load address for PIE binaries.
	if (m_type == ET_DYN)
		addr -= m_load_addr;
	return m_debug.addr_to_source(addr);
}

void kvm_to_dwarf_regs(const kvm_regs& kregs, vsize_t regs[DwarfReg::MAX]) {
	regs[DwarfReg::Rax] = kregs.rax;
	regs[DwarfReg::Rdx] = kregs.rdx;
	regs[DwarfReg::Rcx] = kregs.rdx;
	regs[DwarfReg::Rbx] = kregs.rbx;
	regs[DwarfReg::Rsi] = kregs.rsi;
	regs[DwarfReg::Rdi] = kregs.rdi;
	regs[DwarfReg::Rbp] = kregs.rbp;
	regs[DwarfReg::Rsp] = kregs.rsp;
	regs[DwarfReg::R8]  = kregs.r8;
	regs[DwarfReg::R9]  = kregs.r9;
	regs[DwarfReg::R10] = kregs.r10;
	regs[DwarfReg::R11] = kregs.r11;
	regs[DwarfReg::R12] = kregs.r12;
	regs[DwarfReg::R13] = kregs.r13;
	regs[DwarfReg::R14] = kregs.r14;
	regs[DwarfReg::R15] = kregs.r15;
	regs[DwarfReg::ReturnAddress] = kregs.rip;
}

vector<vaddr_t> ElfParser::get_stacktrace(const kvm_regs& kregs, size_t num_frames,
                                          Mmu& mmu) const
{
	vector<vaddr_t> stacktrace;

	// Transform to dwarf format
	vsize_t regs[DwarfReg::MAX];
	kvm_to_dwarf_regs(kregs, regs);

	// Get limits
	auto limits = section_limits(".text");

	// Loop over stack frames until we're out of limits. For PIE binaries we
	// have to take into account the address the elf is loaded at. ElfDebug
	// expects input ReturnAddress relative to the load address, while output
	// ReturnAddress is the absolute one. Therefore, we have to save the output
	// ReturnAddress, but then we have to substract it the load address before
	// passing it back again to next_frame().
	size_t i = 0;
	do {
		stacktrace.push_back(regs[DwarfReg::ReturnAddress]);
		if (m_type == ET_DYN)
			regs[DwarfReg::ReturnAddress] -= m_load_addr;
	} while (
		++i < num_frames &&
		m_debug.next_frame(regs, mmu) &&
		regs[DwarfReg::ReturnAddress] >= limits.first &&
		regs[DwarfReg::ReturnAddress] < limits.second
	);

	return stacktrace;
}

const ElfParser* elf_with_addr_in_text(const vector<const ElfParser*>& elfs, vaddr_t addr) {
	for (const ElfParser* elf : elfs) {
		auto range = elf->section_limits(".text");
		if (range.first <= addr && addr < range.second)
			return elf;
	}
	return nullptr;
}

vector<pair<vaddr_t, const ElfParser*>> ElfParser::get_stacktrace(
	const vector<const ElfParser*>& elfs,
	const kvm_regs& kregs, size_t num_frames, Mmu& mmu
) {
	vector<pair<vaddr_t, const ElfParser*>> stacktrace;

	// Transform to dwarf format
	vsize_t regs[DwarfReg::MAX];
	kvm_to_dwarf_regs(kregs, regs);

	// For each frame, get the elf it belongs to and use its DWARF info to
	// get the next frame.
	size_t i = 0;
	const ElfParser* elf = nullptr;
	do {
		elf = elf_with_addr_in_text(elfs, regs[DwarfReg::ReturnAddress]);
		if (!elf)
			break;
		stacktrace.push_back({regs[DwarfReg::ReturnAddress], elf});
		if (elf->type() == ET_DYN)
			regs[DwarfReg::ReturnAddress] -= elf->load_addr();
	} while (
		++i < num_frames &&
		elf->m_debug.next_frame(regs, mmu)
	);

	return stacktrace;
}



/* vector<relocation_t> ElfParser::relocations() const {
	return m_relocations;
} */