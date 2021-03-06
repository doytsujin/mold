#include "mold.h"

#include <iostream>

using namespace llvm::ELF;

using llvm::FileOutputBuffer;
using llvm::file_magic;
using llvm::makeArrayRef;
using llvm::object::Archive;
using llvm::opt::InputArgList;

Config config;

//
// Command-line option processing
//

enum {
  OPT_INVALID = 0,
#define OPTION(_1, _2, ID, _4, _5, _6, _7, _8, _9, _10, _11, _12) OPT_##ID,
#include "options.inc"
#undef OPTION
};

// Create prefix string literals used in Options.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td
static const llvm::opt::OptTable::Info opt_info[] = {
#define OPTION(X1, X2, ID, KIND, GROUP, ALIAS, X7, X8, X9, X10, X11, X12)      \
  {X1, X2, X10,         X11,         OPT_##ID, llvm::opt::Option::KIND##Class, \
   X9, X8, OPT_##GROUP, OPT_##ALIAS, X7,       X12},
#include "options.inc"
#undef OPTION
};

class MyOptTable : llvm::opt::OptTable {
public:
  MyOptTable() : OptTable(opt_info) {}
  InputArgList parse(int argc, char **argv);
};

InputArgList MyOptTable::parse(int argc, char **argv) {
  unsigned missingIndex;
  unsigned missingCount;
  SmallVector<const char *, 256> vec(argv, argv + argc);

  InputArgList args = this->ParseArgs(vec, missingIndex, missingCount);
  if (missingCount)
    error(Twine(args.getArgString(missingIndex)) + ": missing argument");

  for (auto *arg : args.filtered(OPT_UNKNOWN))
    error("unknown argument '" + arg->getAsString(args) + "'");
  return args;
}

//
// Main
//

static std::vector<MemoryBufferRef> get_archive_members(MemoryBufferRef mb) {
  std::unique_ptr<Archive> file =
    CHECK(Archive::create(mb), mb.getBufferIdentifier() + ": failed to parse archive");

  std::vector<MemoryBufferRef> vec;

  Error err = Error::success();

  for (const Archive::Child &c : file->children(err)) {
    MemoryBufferRef mbref =
        CHECK(c.getMemoryBufferRef(),
              mb.getBufferIdentifier() +
                  ": could not get the buffer for a child of the archive");
    vec.push_back(mbref);
  }

  if (err)
    error(mb.getBufferIdentifier() + ": Archive::children failed: " +
          toString(std::move(err)));

  file.release(); // leak
  return vec;
}

static void read_file(std::vector<ObjectFile *> &files, StringRef path) {
  MemoryBufferRef mb = readFile(path);

  switch (identify_magic(mb.getBuffer())) {
  case file_magic::archive:
    for (MemoryBufferRef member : get_archive_members(mb))
      files.push_back(new ObjectFile(member, path));
    break;
  case file_magic::elf_relocatable:
    files.push_back(new ObjectFile(mb, ""));
    break;
  default:
    error(path + ": unknown file type");
  }
}

// We want to sort output sections in the following order.
//
//  alloc !writable !exec !tls !nobits
//  alloc !writable !exec !tls  nobits
//  alloc !writable !exec  tls !nobits
//  alloc !writable !exec  tls  nobits
//  alloc !writable  exec
//  alloc  writable !exec !tls !nobits
//  alloc  writable !exec !tls  nobits
//  alloc  writable !exec  tls !nobits
//  alloc  writable !exec  tls nobits
//  alloc  writable  exec
// !alloc
static int get_rank(OutputSection *x) {
  bool alloc = x->hdr.sh_flags & SHF_ALLOC;
  bool writable = x->hdr.sh_flags & SHF_WRITE;
  bool exec = x->hdr.sh_flags & SHF_EXECINSTR;
  bool tls = x->hdr.sh_flags & SHF_TLS;
  bool nobits = x->hdr.sh_type & SHT_NOBITS;
  return (alloc << 5) | (!writable << 4) | (!exec << 3) | (!tls << 2) | !nobits;
}

static std::vector<OutputSection *> get_output_sections() {
  std::vector<OutputSection *> vec;
  for (OutputSection *osec : OutputSection::all_instances)
    if (!osec->chunks.empty())
      vec.push_back(osec);

  std::sort(vec.begin(), vec.end(), [](OutputSection *a, OutputSection *b) {
    int x = get_rank(a);
    int y = get_rank(b);
    if (x != y)
      return x > y;

    // Tie-break to make output deterministic.
    if (a->hdr.sh_flags != b->hdr.sh_flags)
      return a->hdr.sh_flags < b->hdr.sh_flags;
    if (a->hdr.sh_type != b->hdr.sh_type)
      return a->hdr.sh_type < b->hdr.sh_type;
    return a->name < b->name;
  });

  return vec;
}

static std::vector<ELF64LE::Phdr> create_phdrs() {
  return {};
}

static std::vector<ELF64LE::Shdr *>
create_shdrs(ArrayRef<OutputChunk *> output_chunks) {
  static ELF64LE::Shdr null_entry;

  std::vector<ELF64LE::Shdr *> vec;
  vec.push_back(&null_entry);

  for (OutputChunk *chunk : output_chunks)
    if (!chunk->name.empty())
      vec.push_back(&chunk->hdr);
  return vec;
}

static void fill_shdrs(ArrayRef<OutputChunk *> output_chunks) {
  int i = 1;

  for (OutputChunk *chunk : output_chunks) {
    if (chunk->name.empty())
      continue;

    chunk->hdr.sh_name = out::shstrtab->add_string(chunk->name);
    chunk->hdr.sh_offset = chunk->get_offset();
    chunk->hdr.sh_size = chunk->get_size();
    chunk->index = i++;
  }
}

int main(int argc, char **argv) {
  // Parse command line options
  MyOptTable opt_table;
  InputArgList args = opt_table.parse(argc - 1, argv + 1);

  if (auto *arg = args.getLastArg(OPT_o))
    config.output = arg->getValue();
  else
    error("-o option is missing");

  std::vector<ObjectFile *> files;

  llvm::Timer open_timer("opne", "open");
  llvm::Timer parse_timer("parse", "parse");
  llvm::Timer add_symbols_timer("add_symbols", "add_symbols");
  llvm::Timer comdat_timer("comdat", "comdat");
  llvm::Timer bin_sections_timer("bin_sections", "bin_sections");
  llvm::Timer file_offset_timer("file_offset", "file_offset");
  llvm::Timer copy_timer("copy", "copy");
  llvm::Timer reloc_timer("reloc", "reloc");
  llvm::Timer commit_timer("commit", "commit");

  // Open input files
  open_timer.startTimer();
  for (auto *arg : args)
    if (arg->getOption().getID() == OPT_INPUT)
      read_file(files, arg->getValue());
  open_timer.stopTimer();

  // Parse input files
  parse_timer.startTimer();
  for_each(files, [](ObjectFile *file) { file->parse(); });
  parse_timer.stopTimer();

  // Set priorities to files
  for (int i = 0; i < files.size(); i++)
    files[i]->priority = files[i]->is_in_archive() ? i + (1 << 31) : i;

  // Resolve symbols
  add_symbols_timer.startTimer();
  for_each(files, [](ObjectFile *file) { file->register_defined_symbols(); });
  for_each(files, [](ObjectFile *file) { file->register_undefined_symbols(); });
  add_symbols_timer.stopTimer();

  // Eliminate unused archive members.
  files.erase(std::remove_if(files.begin(), files.end(),
                             [](ObjectFile *file){ return !file->is_alive; }),
              files.end());

  // Eliminate duplicate comdat groups.
  comdat_timer.startTimer();
  for (ObjectFile *file : files)
    file->eliminate_duplicate_comdat_groups();
  comdat_timer.stopTimer();

  // Bin input sections into output sections
  bin_sections_timer.startTimer();
  for (ObjectFile *file : files)
    for (InputSection *isec : file->sections)
      if (isec)
        isec->output_section->chunks.push_back(isec);
  bin_sections_timer.stopTimer();

  // Create linker-synthesized sections.
  out::ehdr = new OutputEhdr;
  out::phdr = new OutputPhdr;
  out::shstrtab = new StringTableSection(".shstrtab");

  // Add ELF and program header to the output.
  std::vector<OutputChunk *> output_chunks;
  output_chunks.push_back(out::ehdr);
  output_chunks.push_back(out::phdr);

  // Add .interp section.
  output_chunks.push_back(new InterpSection);

  // Add other output sections.
  for (OutputSection *osec : get_output_sections())
    output_chunks.push_back(osec);

  // Add a string table for section names.
  output_chunks.push_back(out::shstrtab);

  // Create program header contents.
  out::phdr->hdr = create_phdrs();

  // A section header is added to the end of the file by convention.
  out::shdr = new OutputShdr;
  out::shdr->hdr = create_shdrs(output_chunks);
  output_chunks.push_back(out::shdr);

  // Assign offsets to input sections
  file_offset_timer.startTimer();
  uint64_t filesize = 0;
  for (OutputChunk *chunk : output_chunks) {
    chunk->set_offset(filesize);
    filesize += chunk->get_size();
  }
  file_offset_timer.stopTimer();

  // Fill section header.
  fill_shdrs(output_chunks);

  // Create an output file
  Expected<std::unique_ptr<FileOutputBuffer>> buf_or_err =
    FileOutputBuffer::create(config.output, filesize, 0);

  if (!buf_or_err)
    error("failed to open " + config.output + ": " +
          llvm::toString(buf_or_err.takeError()));

  std::unique_ptr<FileOutputBuffer> output_buffer = std::move(*buf_or_err);
  uint8_t *buf = output_buffer->getBufferStart();

  // Copy input sections to the output file
  copy_timer.startTimer();
  for_each(output_chunks, [&](OutputChunk *chunk) { chunk->copy_to(buf); });
  copy_timer.stopTimer();

  reloc_timer.startTimer();
  for_each(output_chunks, [&](OutputChunk *chunk) { chunk->relocate(buf); });
  reloc_timer.stopTimer();

  commit_timer.startTimer();
  if (auto e = output_buffer->commit())
    error("failed to write to the output file: " + toString(std::move(e)));
  commit_timer.stopTimer();

  int num_input_chunks = 0;
  for (ObjectFile *file : files)
    num_input_chunks += file->sections.size();

  llvm::outs() << " input_chunks=" << num_input_chunks << "\n"
               << "output_chunks=" << output_chunks.size() << "\n"
               << "        files=" << files.size() << "\n"
               << "     filesize=" << filesize << "\n"
               << "  num_defined=" << num_defined << "\n"
               << "num_undefined=" << num_undefined << "\n"
               << "   num_relocs=" << num_relocs << "\n";

  llvm::TimerGroup::printAll(llvm::outs());
  llvm::outs().flush();
  _exit(0);
}
