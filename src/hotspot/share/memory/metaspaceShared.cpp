/*
 * Copyright (c) 2012, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jvm_io.h"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/classLoaderDataShared.hpp"
#include "classfile/classListParser.hpp"
#include "classfile/classLoaderExt.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "classfile/lambdaFormInvokers.hpp"
#include "classfile/loaderConstraints.hpp"
#include "classfile/placeholders.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/systemDictionaryShared.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/gcVMOperations.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "interpreter/bytecodes.hpp"
#include "logging/log.hpp"
#include "logging/logMessage.hpp"
#include "memory/archiveBuilder.hpp"
#include "memory/cppVtables.hpp"
#include "memory/dumpAllocStats.hpp"
#include "memory/filemap.hpp"
#include "memory/heapShared.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/metaspaceShared.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/flatArrayKlass.hpp"
#include "oops/inlineKlass.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oopHandle.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vmOperations.hpp"
#include "utilities/align.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/ostream.hpp"
#include "utilities/defaultStream.hpp"
#if INCLUDE_G1GC
#include "gc/g1/g1CollectedHeap.inline.hpp"
#endif

ReservedSpace MetaspaceShared::_symbol_rs;
VirtualSpace MetaspaceShared::_symbol_vs;
bool MetaspaceShared::_has_error_classes;
bool MetaspaceShared::_archive_loading_failed = false;
bool MetaspaceShared::_remapped_readwrite = false;
address MetaspaceShared::_i2i_entry_code_buffers = NULL;
void* MetaspaceShared::_shared_metaspace_static_top = NULL;
intx MetaspaceShared::_relocation_delta;
char* MetaspaceShared::_requested_base_address;
bool MetaspaceShared::_use_optimized_module_handling = true;
bool MetaspaceShared::_use_full_module_graph = true;

// The CDS archive is divided into the following regions:
//     mc  - misc code (the method entry trampolines, c++ vtables)
//     rw  - read-write metadata
//     ro  - read-only metadata and read-only tables
//
//     ca0 - closed archive heap space #0
//     ca1 - closed archive heap space #1 (may be empty)
//     oa0 - open archive heap space #0
//     oa1 - open archive heap space #1 (may be empty)
//
//     bm  - bitmap for relocating the above 7 regions.
//
// The mc, rw, and ro regions are linearly allocated, in the order of mc->rw->ro.
// These regions are aligned with MetaspaceShared::reserved_space_alignment().
//
// These 3 regions are populated in the following steps:
// [0] All classes are loaded in MetaspaceShared::preload_classes(). All metadata are
//     temporarily allocated outside of the shared regions.
// [1] We enter a safepoint and allocate a buffer for the mc/rw/ro regions.
// [2] C++ vtables and method trampolines are copied into the mc region.
// [3] ArchiveBuilder copies RW metadata into the rw region.
// [4] ArchiveBuilder copies RO metadata into the ro region.
// [5] SymbolTable, StringTable, SystemDictionary, and a few other read-only data
//     are copied into the ro region as read-only tables.
//
// The ca0/ca1 and oa0/oa1 regions are populated inside HeapShared::archive_java_heap_objects.
// Their layout is independent of the mc/rw/ro regions.

static DumpRegion _symbol_region("symbols");

char* MetaspaceShared::symbol_space_alloc(size_t num_bytes) {
  return _symbol_region.allocate(num_bytes);
}

size_t MetaspaceShared::reserved_space_alignment() { return os::vm_allocation_granularity(); }

static bool shared_base_valid(char* shared_base) {
#ifdef _LP64
  return CompressedKlassPointers::is_valid_base((address)shared_base);
#else
  return true;
#endif
}

static bool shared_base_too_high(char* specified_base, char* aligned_base, size_t cds_max) {
  if (specified_base != NULL && aligned_base < specified_base) {
    // SharedBaseAddress is very high (e.g., 0xffffffffffffff00) so
    // align_up(SharedBaseAddress, MetaspaceShared::reserved_space_alignment()) has wrapped around.
    return true;
  }
  if (max_uintx - uintx(aligned_base) < uintx(cds_max)) {
    // The end of the archive will wrap around
    return true;
  }

  return false;
}

static char* compute_shared_base(size_t cds_max) {
  char* specified_base = (char*)SharedBaseAddress;
  char* aligned_base = align_up(specified_base, MetaspaceShared::reserved_space_alignment());

  const char* err = NULL;
  if (shared_base_too_high(specified_base, aligned_base, cds_max)) {
    err = "too high";
  } else if (!shared_base_valid(aligned_base)) {
    err = "invalid for this platform";
  } else {
    return aligned_base;
  }

  log_warning(cds)("SharedBaseAddress (" INTPTR_FORMAT ") is %s. Reverted to " INTPTR_FORMAT,
                   p2i((void*)SharedBaseAddress), err,
                   p2i((void*)Arguments::default_SharedBaseAddress()));

  specified_base = (char*)Arguments::default_SharedBaseAddress();
  aligned_base = align_up(specified_base, MetaspaceShared::reserved_space_alignment());

  // Make sure the default value of SharedBaseAddress specified in globals.hpp is sane.
  assert(!shared_base_too_high(specified_base, aligned_base, cds_max), "Sanity");
  assert(shared_base_valid(aligned_base), "Sanity");
  return aligned_base;
}

void MetaspaceShared::initialize_for_static_dump() {
  assert(DumpSharedSpaces, "should be called for dump time only");

  // The max allowed size for CDS archive. We use this to limit SharedBaseAddress
  // to avoid address space wrap around.
  size_t cds_max;
  const size_t reserve_alignment = MetaspaceShared::reserved_space_alignment();

#ifdef _LP64
  const uint64_t UnscaledClassSpaceMax = (uint64_t(max_juint) + 1);
  cds_max = align_down(UnscaledClassSpaceMax, reserve_alignment);
#else
  // We don't support archives larger than 256MB on 32-bit due to limited
  //  virtual address space.
  cds_max = align_down(256*M, reserve_alignment);
#endif

  _requested_base_address = compute_shared_base(cds_max);
  SharedBaseAddress = (size_t)_requested_base_address;

  size_t symbol_rs_size = LP64_ONLY(3 * G) NOT_LP64(128 * M);
  _symbol_rs = ReservedSpace(symbol_rs_size);
  if (!_symbol_rs.is_reserved()) {
    vm_exit_during_initialization("Unable to reserve memory for symbols",
                                  err_msg(SIZE_FORMAT " bytes.", symbol_rs_size));
  }
  _symbol_region.init(&_symbol_rs, &_symbol_vs);
}

// Called by universe_post_init()
void MetaspaceShared::post_initialize(TRAPS) {
  if (UseSharedSpaces) {
    int size = FileMapInfo::get_number_of_shared_paths();
    if (size > 0) {
      SystemDictionaryShared::allocate_shared_data_arrays(size, THREAD);
      if (!DynamicDumpSharedSpaces) {
        FileMapInfo* info;
        if (FileMapInfo::dynamic_info() == NULL) {
          info = FileMapInfo::current_info();
        } else {
          info = FileMapInfo::dynamic_info();
        }
        ClassLoaderExt::init_paths_start_index(info->app_class_paths_start_index());
        ClassLoaderExt::init_app_module_paths_start_index(info->app_module_paths_start_index());
      }
    }
  }
}

static GrowableArrayCHeap<OopHandle, mtClassShared>* _extra_interned_strings = NULL;
static GrowableArrayCHeap<Symbol*, mtClassShared>* _extra_symbols = NULL;

void MetaspaceShared::read_extra_data(const char* filename, TRAPS) {
  _extra_interned_strings = new GrowableArrayCHeap<OopHandle, mtClassShared>(10000);
  _extra_symbols = new GrowableArrayCHeap<Symbol*, mtClassShared>(1000);

  HashtableTextDump reader(filename);
  reader.check_version("VERSION: 1.0");

  while (reader.remain() > 0) {
    int utf8_length;
    int prefix_type = reader.scan_prefix(&utf8_length);
    ResourceMark rm(THREAD);
    if (utf8_length == 0x7fffffff) {
      // buf_len will overflown 32-bit value.
      vm_exit_during_initialization(err_msg("string length too large: %d", utf8_length));
    }
    int buf_len = utf8_length+1;
    char* utf8_buffer = NEW_RESOURCE_ARRAY(char, buf_len);
    reader.get_utf8(utf8_buffer, utf8_length);
    utf8_buffer[utf8_length] = '\0';

    if (prefix_type == HashtableTextDump::SymbolPrefix) {
      _extra_symbols->append(SymbolTable::new_permanent_symbol(utf8_buffer));
    } else{
      assert(prefix_type == HashtableTextDump::StringPrefix, "Sanity");
      oop str = StringTable::intern(utf8_buffer, THREAD);

      if (HAS_PENDING_EXCEPTION) {
        log_warning(cds, heap)("[line %d] extra interned string allocation failed; size too large: %d",
                               reader.last_line_no(), utf8_length);
        CLEAR_PENDING_EXCEPTION;
      } else {
#if INCLUDE_G1GC
        if (UseG1GC) {
          typeArrayOop body = java_lang_String::value(str);
          const HeapRegion* hr = G1CollectedHeap::heap()->heap_region_containing(body);
          if (hr->is_humongous()) {
            // Don't keep it alive, so it will be GC'ed before we dump the strings, in order
            // to maximize free heap space and minimize fragmentation.
            log_warning(cds, heap)("[line %d] extra interned string ignored; size too large: %d",
                                reader.last_line_no(), utf8_length);
            continue;
          }
        }
#endif
        // Make sure this string is included in the dumped interned string table.
        assert(str != NULL, "must succeed");
        _extra_interned_strings->append(OopHandle(Universe::vm_global(), str));
      }
    }
  }
}

// Read/write a data stream for restoring/preserving metadata pointers and
// miscellaneous data from/to the shared archive file.

void MetaspaceShared::serialize(SerializeClosure* soc) {
  int tag = 0;
  soc->do_tag(--tag);

  // Verify the sizes of various metadata in the system.
  soc->do_tag(sizeof(Method));
  soc->do_tag(sizeof(ConstMethod));
  soc->do_tag(arrayOopDesc::base_offset_in_bytes(T_BYTE));
  soc->do_tag(sizeof(ConstantPool));
  soc->do_tag(sizeof(ConstantPoolCache));
  soc->do_tag(objArrayOopDesc::base_offset_in_bytes());
  soc->do_tag(typeArrayOopDesc::base_offset_in_bytes(T_BYTE));
  soc->do_tag(sizeof(Symbol));

  // Dump/restore miscellaneous metadata.
  JavaClasses::serialize_offsets(soc);
  Universe::serialize(soc);
  soc->do_tag(--tag);

  // Dump/restore references to commonly used names and signatures.
  vmSymbols::serialize(soc);
  soc->do_tag(--tag);

  // Dump/restore the symbol/string/subgraph_info tables
  SymbolTable::serialize_shared_table_header(soc);
  StringTable::serialize_shared_table_header(soc);
  HeapShared::serialize_subgraph_info_table_header(soc);
  SystemDictionaryShared::serialize_dictionary_headers(soc);

  InstanceMirrorKlass::serialize_offsets(soc);

  // Dump/restore well known classes (pointers)
  SystemDictionaryShared::serialize_vm_classes(soc);
  soc->do_tag(--tag);

  CppVtables::serialize(soc);
  soc->do_tag(--tag);

  CDS_JAVA_HEAP_ONLY(ClassLoaderDataShared::serialize(soc);)

  soc->do_tag(666);
}

void MetaspaceShared::set_i2i_entry_code_buffers(address b) {
  assert(DumpSharedSpaces, "must be");
  assert(_i2i_entry_code_buffers == NULL, "initialize only once");
  _i2i_entry_code_buffers = b;
}

address MetaspaceShared::i2i_entry_code_buffers() {
  assert(DumpSharedSpaces || UseSharedSpaces, "must be");
  assert(_i2i_entry_code_buffers != NULL, "must already been initialized");
  return _i2i_entry_code_buffers;
}

static void rewrite_nofast_bytecode(const methodHandle& method) {
  BytecodeStream bcs(method);
  while (!bcs.is_last_bytecode()) {
    Bytecodes::Code opcode = bcs.next();
    switch (opcode) {
    case Bytecodes::_getfield:      *bcs.bcp() = Bytecodes::_nofast_getfield;      break;
    case Bytecodes::_putfield:      *bcs.bcp() = Bytecodes::_nofast_putfield;      break;
    case Bytecodes::_aload_0:       *bcs.bcp() = Bytecodes::_nofast_aload_0;       break;
    case Bytecodes::_iload: {
      if (!bcs.is_wide()) {
        *bcs.bcp() = Bytecodes::_nofast_iload;
      }
      break;
    }
    default: break;
    }
  }
}

// [1] Rewrite all bytecodes as needed, so that the ConstMethod* will not be modified
//     at run time by RewriteBytecodes/RewriteFrequentPairs
// [2] Assign a fingerprint, so one doesn't need to be assigned at run-time.
void MetaspaceShared::rewrite_nofast_bytecodes_and_calculate_fingerprints(Thread* thread, InstanceKlass* ik) {
  for (int i = 0; i < ik->methods()->length(); i++) {
    methodHandle m(thread, ik->methods()->at(i));
    rewrite_nofast_bytecode(m);
    Fingerprinter fp(m);
    // The side effect of this call sets method's fingerprint field.
    fp.fingerprint();
  }
}

class VM_PopulateDumpSharedSpace : public VM_GC_Operation {
private:
  GrowableArray<MemRegion> *_closed_archive_heap_regions;
  GrowableArray<MemRegion> *_open_archive_heap_regions;

  GrowableArray<ArchiveHeapOopmapInfo> *_closed_archive_heap_oopmaps;
  GrowableArray<ArchiveHeapOopmapInfo> *_open_archive_heap_oopmaps;

  void dump_java_heap_objects(GrowableArray<Klass*>* klasses) NOT_CDS_JAVA_HEAP_RETURN;
  void dump_archive_heap_oopmaps() NOT_CDS_JAVA_HEAP_RETURN;
  void dump_archive_heap_oopmaps(GrowableArray<MemRegion>* regions,
                                 GrowableArray<ArchiveHeapOopmapInfo>* oopmaps);
  void dump_shared_symbol_table(GrowableArray<Symbol*>* symbols) {
    log_info(cds)("Dumping symbol table ...");
    SymbolTable::write_to_archive(symbols);
  }
  char* dump_read_only_tables();

public:

  VM_PopulateDumpSharedSpace() : VM_GC_Operation(0, /* total collections, ignored */
                                                 GCCause::_archive_time_gc)
  { }

  bool skip_operation() const { return false; }

  VMOp_Type type() const { return VMOp_PopulateDumpSharedSpace; }
  void doit();   // outline because gdb sucks
  bool allow_nested_vm_operations() const { return true; }
}; // class VM_PopulateDumpSharedSpace

class StaticArchiveBuilder : public ArchiveBuilder {
public:
  StaticArchiveBuilder() : ArchiveBuilder() {}

  virtual void iterate_roots(MetaspaceClosure* it, bool is_relocating_pointers) {
    FileMapInfo::metaspace_pointers_do(it, false);
    SystemDictionaryShared::dumptime_classes_do(it);
    Universe::metaspace_pointers_do(it);
    vmSymbols::metaspace_pointers_do(it);

    // The above code should find all the symbols that are referenced by the
    // archived classes. We just need to add the extra symbols which
    // may not be used by any of the archived classes -- these are usually
    // symbols that we anticipate to be used at run time, so we can store
    // them in the RO region, to be shared across multiple processes.
    if (_extra_symbols != NULL) {
      for (int i = 0; i < _extra_symbols->length(); i++) {
        it->push(_extra_symbols->adr_at(i));
      }
    }
  }
};

char* VM_PopulateDumpSharedSpace::dump_read_only_tables() {
  ArchiveBuilder::OtherROAllocMark mark;

  SystemDictionaryShared::write_to_archive();

  // Write the other data to the output array.
  DumpRegion* ro_region = ArchiveBuilder::current()->ro_region();
  char* start = ro_region->top();
  WriteClosure wc(ro_region);
  MetaspaceShared::serialize(&wc);

  // Write the bitmaps for patching the archive heap regions
  _closed_archive_heap_oopmaps = NULL;
  _open_archive_heap_oopmaps = NULL;
  dump_archive_heap_oopmaps();

  return start;
}

void VM_PopulateDumpSharedSpace::doit() {
  HeapShared::run_full_gc_in_vm_thread();

  // We should no longer allocate anything from the metaspace, so that:
  //
  // (1) Metaspace::allocate might trigger GC if we have run out of
  //     committed metaspace, but we can't GC because we're running
  //     in the VM thread.
  // (2) ArchiveBuilder needs to work with a stable set of MetaspaceObjs.
  Metaspace::freeze();
  DEBUG_ONLY(SystemDictionaryShared::NoClassLoadingMark nclm);

  Thread* THREAD = VMThread::vm_thread();

  FileMapInfo::check_nonempty_dir_in_shared_path_table();

  NOT_PRODUCT(SystemDictionary::verify();)
  // The following guarantee is meant to ensure that no loader constraints
  // exist yet, since the constraints table is not shared.  This becomes
  // more important now that we don't re-initialize vtables/itables for
  // shared classes at runtime, where constraints were previously created.
  guarantee(SystemDictionary::constraints()->number_of_entries() == 0,
            "loader constraints are not saved");

  // At this point, many classes have been loaded.
  // Gather systemDictionary classes in a global array and do everything to
  // that so we don't have to walk the SystemDictionary again.
  SystemDictionaryShared::check_excluded_classes();

  StaticArchiveBuilder builder;
  builder.gather_source_objs();
  builder.reserve_buffer();

  builder.init_mc_region();
  char* cloned_vtables = CppVtables::dumptime_init();

  builder.dump_rw_region();
  builder.dump_ro_region();
  builder.relocate_metaspaceobj_embedded_pointers();

  // Dump supported java heap objects
  _closed_archive_heap_regions = NULL;
  _open_archive_heap_regions = NULL;
  dump_java_heap_objects(builder.klasses());

  builder.relocate_roots();
  dump_shared_symbol_table(builder.symbols());

  builder.relocate_vm_classes();

  log_info(cds)("Update method trampolines");
  builder.update_method_trampolines();

  log_info(cds)("Make classes shareable");
  builder.make_klasses_shareable();

  char* serialized_data = dump_read_only_tables();

  SystemDictionaryShared::adjust_lambda_proxy_class_dictionary();

  // The vtable clones contain addresses of the current process.
  // We don't want to write these addresses into the archive. Same for i2i buffer.
  CppVtables::zero_archived_vtables();

  // relocate the data so that it can be mapped to MetaspaceShared::requested_base_address()
  // without runtime relocation.
  builder.relocate_to_requested();

  // Write the archive file
  FileMapInfo* mapinfo = new FileMapInfo(true);
  mapinfo->populate_header(os::vm_allocation_granularity());
  mapinfo->set_serialized_data(serialized_data);
  mapinfo->set_cloned_vtables(cloned_vtables);
  mapinfo->set_i2i_entry_code_buffers(MetaspaceShared::i2i_entry_code_buffers());
  mapinfo->open_for_write();
  builder.write_archive(mapinfo,
                        _closed_archive_heap_regions,
                        _open_archive_heap_regions,
                        _closed_archive_heap_oopmaps,
                        _open_archive_heap_oopmaps);

  if (PrintSystemDictionaryAtExit) {
    SystemDictionary::print();
  }

  if (AllowArchivingWithJavaAgent) {
    warning("This archive was created with AllowArchivingWithJavaAgent. It should be used "
            "for testing purposes only and should not be used in a production environment");
  }

  // There may be pending VM operations. We have changed some global states
  // (such as vmClasses::_klasses) that may cause these VM operations
  // to fail. For safety, forget these operations and exit the VM directly.
  vm_direct_exit(0);
}

static GrowableArray<ClassLoaderData*>* _loaded_cld = NULL;

class CollectCLDClosure : public CLDClosure {
  void do_cld(ClassLoaderData* cld) {
    if (_loaded_cld == NULL) {
      _loaded_cld = new (ResourceObj::C_HEAP, mtClassShared)GrowableArray<ClassLoaderData*>(10, mtClassShared);
    }
    if (!cld->is_unloading()) {
      cld->inc_keep_alive();
      _loaded_cld->append(cld);
    }
  }
};

bool MetaspaceShared::linking_required(InstanceKlass* ik) {
  // For dynamic CDS dump, only link classes loaded by the builtin class loaders.
  return DumpSharedSpaces ? true : !ik->is_shared_unregistered_class();
}

bool MetaspaceShared::link_class_for_cds(InstanceKlass* ik, TRAPS) {
  // Link the class to cause the bytecodes to be rewritten and the
  // cpcache to be created. Class verification is done according
  // to -Xverify setting.
  bool res = MetaspaceShared::try_link_class(ik, THREAD);
  guarantee(!HAS_PENDING_EXCEPTION, "exception in link_class");

  if (DumpSharedSpaces) {
    // The following function is used to resolve all Strings in the statically
    // dumped classes to archive all the Strings. The archive heap is not supported
    // for the dynamic archive.
    ik->constants()->resolve_class_constants(THREAD);
  }
  return res;
}

void MetaspaceShared::link_and_cleanup_shared_classes(TRAPS) {
  // Collect all loaded ClassLoaderData.
  CollectCLDClosure collect_cld;
  {
    MutexLocker lock(ClassLoaderDataGraph_lock);
    ClassLoaderDataGraph::loaded_cld_do(&collect_cld);
  }

  while (true) {
    bool has_linked = false;
    for (int i = 0; i < _loaded_cld->length(); i++) {
      ClassLoaderData* cld = _loaded_cld->at(i);
      for (Klass* klass = cld->klasses(); klass != NULL; klass = klass->next_link()) {
        if (klass->is_instance_klass()) {
          InstanceKlass* ik = InstanceKlass::cast(klass);
          if (linking_required(ik)) {
            has_linked |= link_class_for_cds(ik, THREAD);
          }
        }
      }
    }

    if (!has_linked) {
      break;
    }
    // Class linking includes verification which may load more classes.
    // Keep scanning until we have linked no more classes.
  }

  for (int i = 0; i < _loaded_cld->length(); i++) {
    ClassLoaderData* cld = _loaded_cld->at(i);
    cld->dec_keep_alive();
  }
}

void MetaspaceShared::prepare_for_dumping() {
  Arguments::assert_is_dumping_archive();
  Arguments::check_unsupported_dumping_properties();

  EXCEPTION_MARK;
  ClassLoader::initialize_shared_path(THREAD);
  if (HAS_PENDING_EXCEPTION) {
    java_lang_Throwable::print(PENDING_EXCEPTION, tty);
    vm_exit_during_initialization("ClassLoader::initialize_shared_path() failed unexpectedly");
  }
}

// Preload classes from a list, populate the shared spaces and dump to a
// file.
void MetaspaceShared::preload_and_dump(TRAPS) {
  { TraceTime timer("Dump Shared Spaces", TRACETIME_LOG(Info, startuptime));
    ResourceMark rm(THREAD);
    char class_list_path_str[JVM_MAXPATHLEN];
    // Preload classes to be shared.
    const char* class_list_path;
    if (SharedClassListFile == NULL) {
      // Construct the path to the class list (in jre/lib)
      // Walk up two directories from the location of the VM and
      // optionally tack on "lib" (depending on platform)
      os::jvm_path(class_list_path_str, sizeof(class_list_path_str));
      for (int i = 0; i < 3; i++) {
        char *end = strrchr(class_list_path_str, *os::file_separator());
        if (end != NULL) *end = '\0';
      }
      int class_list_path_len = (int)strlen(class_list_path_str);
      if (class_list_path_len >= 3) {
        if (strcmp(class_list_path_str + class_list_path_len - 3, "lib") != 0) {
          if (class_list_path_len < JVM_MAXPATHLEN - 4) {
            jio_snprintf(class_list_path_str + class_list_path_len,
                         sizeof(class_list_path_str) - class_list_path_len,
                         "%slib", os::file_separator());
            class_list_path_len += 4;
          }
        }
      }
      if (class_list_path_len < JVM_MAXPATHLEN - 10) {
        jio_snprintf(class_list_path_str + class_list_path_len,
                     sizeof(class_list_path_str) - class_list_path_len,
                     "%sclasslist", os::file_separator());
      }
      class_list_path = class_list_path_str;
    } else {
      class_list_path = SharedClassListFile;
    }

    log_info(cds)("Loading classes to share ...");
    _has_error_classes = false;
    int class_count = preload_classes(class_list_path, THREAD);
    if (ExtraSharedClassListFile) {
      class_count += preload_classes(ExtraSharedClassListFile, THREAD);
    }
    log_info(cds)("Loading classes to share: done.");

    log_info(cds)("Shared spaces: preloaded %d classes", class_count);

    if (SharedArchiveConfigFile) {
      log_info(cds)("Reading extra data from %s ...", SharedArchiveConfigFile);
      read_extra_data(SharedArchiveConfigFile, THREAD);
      log_info(cds)("Reading extra data: done.");
    }

    if (LambdaFormInvokers::lambdaform_lines() != NULL) {
      log_info(cds)("Regenerate MethodHandle Holder classes...");
      LambdaFormInvokers::regenerate_holder_classes(THREAD);
      log_info(cds)("Regenerate MethodHandle Holder classes done.");
    }

    HeapShared::init_for_dumping(THREAD);

    // exercise the manifest processing code to ensure classes used by CDS are always archived
    SystemDictionaryShared::create_jar_manifest("Manifest-Version: 1.0\n", strlen("Manifest-Version: 1.0\n"), THREAD);
    // Rewrite and link classes
    log_info(cds)("Rewriting and linking classes ...");

    // Link any classes which got missed. This would happen if we have loaded classes that
    // were not explicitly specified in the classlist. E.g., if an interface implemented by class K
    // fails verification, all other interfaces that were not specified in the classlist but
    // are implemented by K are not verified.
    link_and_cleanup_shared_classes(CATCH);
    log_info(cds)("Rewriting and linking classes: done");

#if INCLUDE_CDS_JAVA_HEAP
    if (use_full_module_graph()) {
      HeapShared::reset_archived_object_states(THREAD);
    }
#endif

    VM_PopulateDumpSharedSpace op;
    VMThread::execute(&op);
  }
}


int MetaspaceShared::preload_classes(const char* class_list_path, TRAPS) {
  ClassListParser parser(class_list_path);
  int class_count = 0;

  while (parser.parse_one_line()) {
    if (parser.lambda_form_line()) {
      continue;
    }
    Klass* klass = parser.load_current_class(THREAD);
    if (HAS_PENDING_EXCEPTION) {
      if (klass == NULL &&
          (PENDING_EXCEPTION->klass()->name() == vmSymbols::java_lang_ClassNotFoundException())) {
        // print a warning only when the pending exception is class not found
        log_warning(cds)("Preload Warning: Cannot find %s", parser.current_class_name());
      }
      CLEAR_PENDING_EXCEPTION;
    }
    if (klass != NULL) {
      if (log_is_enabled(Trace, cds)) {
        ResourceMark rm(THREAD);
        log_trace(cds)("Shared spaces preloaded: %s", klass->external_name());
      }

      if (klass->is_instance_klass()) {
        InstanceKlass* ik = InstanceKlass::cast(klass);

        // Link the class to cause the bytecodes to be rewritten and the
        // cpcache to be created. The linking is done as soon as classes
        // are loaded in order that the related data structures (klass and
        // cpCache) are located together.
        try_link_class(ik, THREAD);
        guarantee(!HAS_PENDING_EXCEPTION, "exception in link_class");
      }

      class_count++;
    }
  }

  return class_count;
}

// Returns true if the class's status has changed
bool MetaspaceShared::try_link_class(InstanceKlass* ik, TRAPS) {
  Arguments::assert_is_dumping_archive();
  if (ik->is_loaded() && !ik->is_linked() &&
      !SystemDictionaryShared::has_class_failed_verification(ik)) {
    bool saved = BytecodeVerificationLocal;
    if (ik->is_shared_unregistered_class() && ik->class_loader() == NULL) {
      // The verification decision is based on BytecodeVerificationRemote
      // for non-system classes. Since we are using the NULL classloader
      // to load non-system classes for customized class loaders during dumping,
      // we need to temporarily change BytecodeVerificationLocal to be the same as
      // BytecodeVerificationRemote. Note this can cause the parent system
      // classes also being verified. The extra overhead is acceptable during
      // dumping.
      BytecodeVerificationLocal = BytecodeVerificationRemote;
    }
    ik->link_class(THREAD);
    if (HAS_PENDING_EXCEPTION) {
      ResourceMark rm(THREAD);
      log_warning(cds)("Preload Warning: Verification failed for %s",
                    ik->external_name());
      CLEAR_PENDING_EXCEPTION;
      SystemDictionaryShared::set_class_has_failed_verification(ik);
      _has_error_classes = true;
    }
    BytecodeVerificationLocal = saved;
    return true;
  } else {
    return false;
  }
}

#if INCLUDE_CDS_JAVA_HEAP
void VM_PopulateDumpSharedSpace::dump_java_heap_objects(GrowableArray<Klass*>* klasses) {
  if(!HeapShared::is_heap_object_archiving_allowed()) {
    log_info(cds)(
      "Archived java heap is not supported as UseG1GC, "
      "UseCompressedOops and UseCompressedClassPointers are required."
      "Current settings: UseG1GC=%s, UseCompressedOops=%s, UseCompressedClassPointers=%s.",
      BOOL_TO_STR(UseG1GC), BOOL_TO_STR(UseCompressedOops),
      BOOL_TO_STR(UseCompressedClassPointers));
    return;
  }
  // Find all the interned strings that should be dumped.
  int i;
  for (i = 0; i < klasses->length(); i++) {
    Klass* k = klasses->at(i);
    if (k->is_instance_klass()) {
      InstanceKlass* ik = InstanceKlass::cast(k);
      ik->constants()->add_dumped_interned_strings();
    }
  }
  if (_extra_interned_strings != NULL) {
    for (i = 0; i < _extra_interned_strings->length(); i ++) {
      OopHandle string = _extra_interned_strings->at(i);
      HeapShared::add_to_dumped_interned_strings(string.resolve());
    }
  }

  // The closed and open archive heap space has maximum two regions.
  // See FileMapInfo::write_archive_heap_regions() for details.
  _closed_archive_heap_regions = new GrowableArray<MemRegion>(2);
  _open_archive_heap_regions = new GrowableArray<MemRegion>(2);
  HeapShared::archive_java_heap_objects(_closed_archive_heap_regions,
                                        _open_archive_heap_regions);
  ArchiveBuilder::OtherROAllocMark mark;
  HeapShared::write_subgraph_info_table();
}

void VM_PopulateDumpSharedSpace::dump_archive_heap_oopmaps() {
  if (HeapShared::is_heap_object_archiving_allowed()) {
    _closed_archive_heap_oopmaps = new GrowableArray<ArchiveHeapOopmapInfo>(2);
    dump_archive_heap_oopmaps(_closed_archive_heap_regions, _closed_archive_heap_oopmaps);

    _open_archive_heap_oopmaps = new GrowableArray<ArchiveHeapOopmapInfo>(2);
    dump_archive_heap_oopmaps(_open_archive_heap_regions, _open_archive_heap_oopmaps);
  }
}

void VM_PopulateDumpSharedSpace::dump_archive_heap_oopmaps(GrowableArray<MemRegion>* regions,
                                                           GrowableArray<ArchiveHeapOopmapInfo>* oopmaps) {
  for (int i=0; i<regions->length(); i++) {
    ResourceBitMap oopmap = HeapShared::calculate_oopmap(regions->at(i));
    size_t size_in_bits = oopmap.size();
    size_t size_in_bytes = oopmap.size_in_bytes();
    uintptr_t* buffer = (uintptr_t*)NEW_C_HEAP_ARRAY(char, size_in_bytes, mtInternal);
    oopmap.write_to(buffer, size_in_bytes);
    log_info(cds, heap)("Oopmap = " INTPTR_FORMAT " (" SIZE_FORMAT_W(6) " bytes) for heap region "
                        INTPTR_FORMAT " (" SIZE_FORMAT_W(8) " bytes)",
                        p2i(buffer), size_in_bytes,
                        p2i(regions->at(i).start()), regions->at(i).byte_size());

    ArchiveHeapOopmapInfo info;
    info._oopmap = (address)buffer;
    info._oopmap_size_in_bits = size_in_bits;
    info._oopmap_size_in_bytes = size_in_bytes;
    oopmaps->append(info);
  }
}
#endif // INCLUDE_CDS_JAVA_HEAP

void MetaspaceShared::set_shared_metaspace_range(void* base, void *static_top, void* top) {
  assert(base <= static_top && static_top <= top, "must be");
  _shared_metaspace_static_top = static_top;
  MetaspaceObj::set_shared_metaspace_range(base, top);
}

// Return true if given address is in the misc data region
bool MetaspaceShared::is_in_shared_region(const void* p, int idx) {
  return UseSharedSpaces && FileMapInfo::current_info()->is_in_shared_region(p, idx);
}

bool MetaspaceShared::is_in_trampoline_frame(address addr) {
  if (UseSharedSpaces && is_in_shared_region(addr, MetaspaceShared::mc)) {
    return true;
  }
  return false;
}

bool MetaspaceShared::is_shared_dynamic(void* p) {
  if ((p < MetaspaceObj::shared_metaspace_top()) &&
      (p >= _shared_metaspace_static_top)) {
    return true;
  } else {
    return false;
  }
}

void MetaspaceShared::initialize_runtime_shared_and_meta_spaces() {
  assert(UseSharedSpaces, "Must be called when UseSharedSpaces is enabled");
  MapArchiveResult result = MAP_ARCHIVE_OTHER_FAILURE;

  FileMapInfo* static_mapinfo = open_static_archive();
  FileMapInfo* dynamic_mapinfo = NULL;

  if (static_mapinfo != NULL) {
    dynamic_mapinfo = open_dynamic_archive();

    // First try to map at the requested address
    result = map_archives(static_mapinfo, dynamic_mapinfo, true);
    if (result == MAP_ARCHIVE_MMAP_FAILURE) {
      // Mapping has failed (probably due to ASLR). Let's map at an address chosen
      // by the OS.
      log_info(cds)("Try to map archive(s) at an alternative address");
      result = map_archives(static_mapinfo, dynamic_mapinfo, false);
    }
  }

  if (result == MAP_ARCHIVE_SUCCESS) {
    bool dynamic_mapped = (dynamic_mapinfo != NULL && dynamic_mapinfo->is_mapped());
    char* cds_base = static_mapinfo->mapped_base();
    char* cds_end =  dynamic_mapped ? dynamic_mapinfo->mapped_end() : static_mapinfo->mapped_end();
    set_shared_metaspace_range(cds_base, static_mapinfo->mapped_end(), cds_end);
    _relocation_delta = static_mapinfo->relocation_delta();
    if (dynamic_mapped) {
      FileMapInfo::set_shared_path_table(dynamic_mapinfo);
    } else {
      FileMapInfo::set_shared_path_table(static_mapinfo);
    }
    _requested_base_address = static_mapinfo->requested_base_address();
  } else {
    set_shared_metaspace_range(NULL, NULL, NULL);
    UseSharedSpaces = false;
    FileMapInfo::fail_continue("Unable to map shared spaces");
    if (PrintSharedArchiveAndExit) {
      vm_exit_during_initialization("Unable to use shared archive.");
    }
  }

  if (static_mapinfo != NULL && !static_mapinfo->is_mapped()) {
    delete static_mapinfo;
  }
  if (dynamic_mapinfo != NULL && !dynamic_mapinfo->is_mapped()) {
    delete dynamic_mapinfo;
  }
}

FileMapInfo* MetaspaceShared::open_static_archive() {
  FileMapInfo* mapinfo = new FileMapInfo(true);
  if (!mapinfo->initialize()) {
    delete(mapinfo);
    return NULL;
  }
  return mapinfo;
}

FileMapInfo* MetaspaceShared::open_dynamic_archive() {
  if (DynamicDumpSharedSpaces) {
    return NULL;
  }
  if (Arguments::GetSharedDynamicArchivePath() == NULL) {
    return NULL;
  }

  FileMapInfo* mapinfo = new FileMapInfo(false);
  if (!mapinfo->initialize()) {
    delete(mapinfo);
    return NULL;
  }
  return mapinfo;
}

// use_requested_addr:
//  true  = map at FileMapHeader::_requested_base_address
//  false = map at an alternative address picked by OS.
MapArchiveResult MetaspaceShared::map_archives(FileMapInfo* static_mapinfo, FileMapInfo* dynamic_mapinfo,
                                               bool use_requested_addr) {
  if (use_requested_addr && static_mapinfo->requested_base_address() == NULL) {
    log_info(cds)("Archive(s) were created with -XX:SharedBaseAddress=0. Always map at os-selected address.");
    return MAP_ARCHIVE_MMAP_FAILURE;
  }

  PRODUCT_ONLY(if (ArchiveRelocationMode == 1 && use_requested_addr) {
      // For product build only -- this is for benchmarking the cost of doing relocation.
      // For debug builds, the check is done below, after reserving the space, for better test coverage
      // (see comment below).
      log_info(cds)("ArchiveRelocationMode == 1: always map archive(s) at an alternative address");
      return MAP_ARCHIVE_MMAP_FAILURE;
    });

  if (ArchiveRelocationMode == 2 && !use_requested_addr) {
    log_info(cds)("ArchiveRelocationMode == 2: never map archive(s) at an alternative address");
    return MAP_ARCHIVE_MMAP_FAILURE;
  };

  if (dynamic_mapinfo != NULL) {
    // Ensure that the OS won't be able to allocate new memory spaces between the two
    // archives, or else it would mess up the simple comparision in MetaspaceObj::is_shared().
    assert(static_mapinfo->mapping_end_offset() == dynamic_mapinfo->mapping_base_offset(), "no gap");
  }

  ReservedSpace total_space_rs, archive_space_rs, class_space_rs;
  MapArchiveResult result = MAP_ARCHIVE_OTHER_FAILURE;
  char* mapped_base_address = reserve_address_space_for_archives(static_mapinfo,
                                                                 dynamic_mapinfo,
                                                                 use_requested_addr,
                                                                 total_space_rs,
                                                                 archive_space_rs,
                                                                 class_space_rs);
  if (mapped_base_address == NULL) {
    result = MAP_ARCHIVE_MMAP_FAILURE;
    log_debug(cds)("Failed to reserve spaces (use_requested_addr=%u)", (unsigned)use_requested_addr);
  } else {

#ifdef ASSERT
    // Some sanity checks after reserving address spaces for archives
    //  and class space.
    assert(archive_space_rs.is_reserved(), "Sanity");
    if (Metaspace::using_class_space()) {
      // Class space must closely follow the archive space. Both spaces
      //  must be aligned correctly.
      assert(class_space_rs.is_reserved(),
             "A class space should have been reserved");
      assert(class_space_rs.base() >= archive_space_rs.end(),
             "class space should follow the cds archive space");
      assert(is_aligned(archive_space_rs.base(),
                        MetaspaceShared::reserved_space_alignment()),
             "Archive space misaligned");
      assert(is_aligned(class_space_rs.base(),
                        Metaspace::reserve_alignment()),
             "class space misaligned");
    }
#endif // ASSERT

    log_debug(cds)("Reserved archive_space_rs     [" INTPTR_FORMAT " - " INTPTR_FORMAT "] (" SIZE_FORMAT ") bytes",
                   p2i(archive_space_rs.base()), p2i(archive_space_rs.end()), archive_space_rs.size());
    log_debug(cds)("Reserved class_space_rs [" INTPTR_FORMAT " - " INTPTR_FORMAT "] (" SIZE_FORMAT ") bytes",
                   p2i(class_space_rs.base()), p2i(class_space_rs.end()), class_space_rs.size());

    if (MetaspaceShared::use_windows_memory_mapping()) {
      // We have now reserved address space for the archives, and will map in
      //  the archive files into this space.
      //
      // Special handling for Windows: on Windows we cannot map a file view
      //  into an existing memory mapping. So, we unmap the address range we
      //  just reserved again, which will make it available for mapping the
      //  archives.
      // Reserving this range has not been for naught however since it makes
      //  us reasonably sure the address range is available.
      //
      // But still it may fail, since between unmapping the range and mapping
      //  in the archive someone else may grab the address space. Therefore
      //  there is a fallback in FileMap::map_region() where we just read in
      //  the archive files sequentially instead of mapping it in. We couple
      //  this with use_requested_addr, since we're going to patch all the
      //  pointers anyway so there's no benefit to mmap.
      if (use_requested_addr) {
        assert(!total_space_rs.is_reserved(), "Should not be reserved for Windows");
        log_info(cds)("Windows mmap workaround: releasing archive space.");
        archive_space_rs.release();
      }
    }
    MapArchiveResult static_result = map_archive(static_mapinfo, mapped_base_address, archive_space_rs);
    MapArchiveResult dynamic_result = (static_result == MAP_ARCHIVE_SUCCESS) ?
                                     map_archive(dynamic_mapinfo, mapped_base_address, archive_space_rs) : MAP_ARCHIVE_OTHER_FAILURE;

    DEBUG_ONLY(if (ArchiveRelocationMode == 1 && use_requested_addr) {
      // This is for simulating mmap failures at the requested address. In
      //  debug builds, we do it here (after all archives have possibly been
      //  mapped), so we can thoroughly test the code for failure handling
      //  (releasing all allocated resource, etc).
      log_info(cds)("ArchiveRelocationMode == 1: always map archive(s) at an alternative address");
      if (static_result == MAP_ARCHIVE_SUCCESS) {
        static_result = MAP_ARCHIVE_MMAP_FAILURE;
      }
      if (dynamic_result == MAP_ARCHIVE_SUCCESS) {
        dynamic_result = MAP_ARCHIVE_MMAP_FAILURE;
      }
    });

    if (static_result == MAP_ARCHIVE_SUCCESS) {
      if (dynamic_result == MAP_ARCHIVE_SUCCESS) {
        result = MAP_ARCHIVE_SUCCESS;
      } else if (dynamic_result == MAP_ARCHIVE_OTHER_FAILURE) {
        assert(dynamic_mapinfo != NULL && !dynamic_mapinfo->is_mapped(), "must have failed");
        // No need to retry mapping the dynamic archive again, as it will never succeed
        // (bad file, etc) -- just keep the base archive.
        log_warning(cds, dynamic)("Unable to use shared archive. The top archive failed to load: %s",
                                  dynamic_mapinfo->full_path());
        result = MAP_ARCHIVE_SUCCESS;
        // TODO, we can give the unused space for the dynamic archive to class_space_rs, but there's no
        // easy API to do that right now.
      } else {
        result = MAP_ARCHIVE_MMAP_FAILURE;
      }
    } else if (static_result == MAP_ARCHIVE_OTHER_FAILURE) {
      result = MAP_ARCHIVE_OTHER_FAILURE;
    } else {
      result = MAP_ARCHIVE_MMAP_FAILURE;
    }
  }

  if (result == MAP_ARCHIVE_SUCCESS) {
    SharedBaseAddress = (size_t)mapped_base_address;
    LP64_ONLY({
        if (Metaspace::using_class_space()) {
          // Set up ccs in metaspace.
          Metaspace::initialize_class_space(class_space_rs);

          // Set up compressed Klass pointer encoding: the encoding range must
          //  cover both archive and class space.
          address cds_base = (address)static_mapinfo->mapped_base();
          address ccs_end = (address)class_space_rs.end();
          assert(ccs_end > cds_base, "Sanity check");
          CompressedKlassPointers::initialize(cds_base, ccs_end - cds_base);

          // map_heap_regions() compares the current narrow oop and klass encodings
          // with the archived ones, so it must be done after all encodings are determined.
          static_mapinfo->map_heap_regions();
        }
      });
    log_info(cds)("optimized module handling: %s", MetaspaceShared::use_optimized_module_handling() ? "enabled" : "disabled");
    log_info(cds)("full module graph: %s", MetaspaceShared::use_full_module_graph() ? "enabled" : "disabled");
  } else {
    unmap_archive(static_mapinfo);
    unmap_archive(dynamic_mapinfo);
    release_reserved_spaces(total_space_rs, archive_space_rs, class_space_rs);
  }

  return result;
}


// This will reserve two address spaces suitable to house Klass structures, one
//  for the cds archives (static archive and optionally dynamic archive) and
//  optionally one move for ccs.
//
// Since both spaces must fall within the compressed class pointer encoding
//  range, they are allocated close to each other.
//
// Space for archives will be reserved first, followed by a potential gap,
//  followed by the space for ccs:
//
// +-- Base address             A        B                     End
// |                            |        |                      |
// v                            v        v                      v
// +-------------+--------------+        +----------------------+
// | static arc  | [dyn. arch]  | [gap]  | compr. class space   |
// +-------------+--------------+        +----------------------+
//
// (The gap may result from different alignment requirements between metaspace
//  and CDS)
//
// If UseCompressedClassPointers is disabled, only one address space will be
//  reserved:
//
// +-- Base address             End
// |                            |
// v                            v
// +-------------+--------------+
// | static arc  | [dyn. arch]  |
// +-------------+--------------+
//
// Base address: If use_archive_base_addr address is true, the Base address is
//  determined by the address stored in the static archive. If
//  use_archive_base_addr address is false, this base address is determined
//  by the platform.
//
// If UseCompressedClassPointers=1, the range encompassing both spaces will be
//  suitable to en/decode narrow Klass pointers: the base will be valid for
//  encoding, the range [Base, End) not surpass KlassEncodingMetaspaceMax.
//
// Return:
//
// - On success:
//    - total_space_rs will be reserved as whole for archive_space_rs and
//      class_space_rs if UseCompressedClassPointers is true.
//      On Windows, try reserve archive_space_rs and class_space_rs
//      separately first if use_archive_base_addr is true.
//    - archive_space_rs will be reserved and large enough to host static and
//      if needed dynamic archive: [Base, A).
//      archive_space_rs.base and size will be aligned to CDS reserve
//      granularity.
//    - class_space_rs: If UseCompressedClassPointers=1, class_space_rs will
//      be reserved. Its start address will be aligned to metaspace reserve
//      alignment, which may differ from CDS alignment. It will follow the cds
//      archive space, close enough such that narrow class pointer encoding
//      covers both spaces.
//      If UseCompressedClassPointers=0, class_space_rs remains unreserved.
// - On error: NULL is returned and the spaces remain unreserved.
char* MetaspaceShared::reserve_address_space_for_archives(FileMapInfo* static_mapinfo,
                                                          FileMapInfo* dynamic_mapinfo,
                                                          bool use_archive_base_addr,
                                                          ReservedSpace& total_space_rs,
                                                          ReservedSpace& archive_space_rs,
                                                          ReservedSpace& class_space_rs) {

  address const base_address = (address) (use_archive_base_addr ? static_mapinfo->requested_base_address() : NULL);
  const size_t archive_space_alignment = MetaspaceShared::reserved_space_alignment();

  // Size and requested location of the archive_space_rs (for both static and dynamic archives)
  assert(static_mapinfo->mapping_base_offset() == 0, "Must be");
  size_t archive_end_offset  = (dynamic_mapinfo == NULL) ? static_mapinfo->mapping_end_offset() : dynamic_mapinfo->mapping_end_offset();
  size_t archive_space_size = align_up(archive_end_offset, archive_space_alignment);

  // If a base address is given, it must have valid alignment and be suitable as encoding base.
  if (base_address != NULL) {
    assert(is_aligned(base_address, archive_space_alignment),
           "Archive base address invalid: " PTR_FORMAT ".", p2i(base_address));
    if (Metaspace::using_class_space()) {
      assert(CompressedKlassPointers::is_valid_base(base_address),
             "Archive base address invalid: " PTR_FORMAT ".", p2i(base_address));
    }
  }

  if (!Metaspace::using_class_space()) {
    // Get the simple case out of the way first:
    // no compressed class space, simple allocation.
    archive_space_rs = ReservedSpace(archive_space_size, archive_space_alignment,
                                     false /* bool large */, (char*)base_address);
    if (archive_space_rs.is_reserved()) {
      assert(base_address == NULL ||
             (address)archive_space_rs.base() == base_address, "Sanity");
      // Register archive space with NMT.
      MemTracker::record_virtual_memory_type(archive_space_rs.base(), mtClassShared);
      return archive_space_rs.base();
    }
    return NULL;
  }

#ifdef _LP64

  // Complex case: two spaces adjacent to each other, both to be addressable
  //  with narrow class pointers.
  // We reserve the whole range spanning both spaces, then split that range up.

  const size_t class_space_alignment = Metaspace::reserve_alignment();

  // To simplify matters, lets assume that metaspace alignment will always be
  //  equal or a multiple of archive alignment.
  assert(is_power_of_2(class_space_alignment) &&
                       is_power_of_2(archive_space_alignment) &&
                       class_space_alignment >= archive_space_alignment,
                       "Sanity");

  const size_t class_space_size = CompressedClassSpaceSize;
  assert(CompressedClassSpaceSize > 0 &&
         is_aligned(CompressedClassSpaceSize, class_space_alignment),
         "CompressedClassSpaceSize malformed: "
         SIZE_FORMAT, CompressedClassSpaceSize);

  const size_t ccs_begin_offset = align_up(base_address + archive_space_size,
                                           class_space_alignment) - base_address;
  const size_t gap_size = ccs_begin_offset - archive_space_size;

  const size_t total_range_size =
      align_up(archive_space_size + gap_size + class_space_size,
               os::vm_allocation_granularity());

  assert(total_range_size > ccs_begin_offset, "must be");
  if (use_windows_memory_mapping() && use_archive_base_addr) {
    if (base_address != nullptr) {
      // On Windows, we cannot safely split a reserved memory space into two (see JDK-8255917).
      // Hence, we optimistically reserve archive space and class space side-by-side. We only
      // do this for use_archive_base_addr=true since for use_archive_base_addr=false case
      // caller will not split the combined space for mapping, instead read the archive data
      // via sequential file IO.
      address ccs_base = base_address + archive_space_size + gap_size;
      archive_space_rs = ReservedSpace(archive_space_size, archive_space_alignment,
                                       false /* large */, (char*)base_address);
      class_space_rs   = ReservedSpace(class_space_size, class_space_alignment,
                                       false /* large */, (char*)ccs_base);
    }
    if (!archive_space_rs.is_reserved() || !class_space_rs.is_reserved()) {
      release_reserved_spaces(total_space_rs, archive_space_rs, class_space_rs);
      return NULL;
    }
  } else {
    if (use_archive_base_addr && base_address != nullptr) {
      total_space_rs = ReservedSpace(total_range_size, archive_space_alignment,
                                     false /* bool large */, (char*) base_address);
    } else {
      // Reserve at any address, but leave it up to the platform to choose a good one.
      total_space_rs = Metaspace::reserve_address_space_for_compressed_classes(total_range_size);
    }

    if (!total_space_rs.is_reserved()) {
      return NULL;
    }

    // Paranoid checks:
    assert(base_address == NULL || (address)total_space_rs.base() == base_address,
           "Sanity (" PTR_FORMAT " vs " PTR_FORMAT ")", p2i(base_address), p2i(total_space_rs.base()));
    assert(is_aligned(total_space_rs.base(), archive_space_alignment), "Sanity");
    assert(total_space_rs.size() == total_range_size, "Sanity");
    assert(CompressedKlassPointers::is_valid_base((address)total_space_rs.base()), "Sanity");

    // Now split up the space into ccs and cds archive. For simplicity, just leave
    //  the gap reserved at the end of the archive space. Do not do real splitting.
    archive_space_rs = total_space_rs.first_part(ccs_begin_offset,
                                                 (size_t)os::vm_allocation_granularity());
    class_space_rs = total_space_rs.last_part(ccs_begin_offset);
    MemTracker::record_virtual_memory_split_reserved(total_space_rs.base(), total_space_rs.size(),
                                                     ccs_begin_offset);
  }
  assert(is_aligned(archive_space_rs.base(), archive_space_alignment), "Sanity");
  assert(is_aligned(archive_space_rs.size(), archive_space_alignment), "Sanity");
  assert(is_aligned(class_space_rs.base(), class_space_alignment), "Sanity");
  assert(is_aligned(class_space_rs.size(), class_space_alignment), "Sanity");

  // NMT: fix up the space tags
  MemTracker::record_virtual_memory_type(archive_space_rs.base(), mtClassShared);
  MemTracker::record_virtual_memory_type(class_space_rs.base(), mtClass);

  return archive_space_rs.base();

#else
  ShouldNotReachHere();
  return NULL;
#endif

}

void MetaspaceShared::release_reserved_spaces(ReservedSpace& total_space_rs,
                                              ReservedSpace& archive_space_rs,
                                              ReservedSpace& class_space_rs) {
  if (total_space_rs.is_reserved()) {
    log_debug(cds)("Released shared space (archive + class) " INTPTR_FORMAT, p2i(total_space_rs.base()));
    total_space_rs.release();
  } else {
    if (archive_space_rs.is_reserved()) {
      log_debug(cds)("Released shared space (archive) " INTPTR_FORMAT, p2i(archive_space_rs.base()));
      archive_space_rs.release();
    }
    if (class_space_rs.is_reserved()) {
      log_debug(cds)("Released shared space (classes) " INTPTR_FORMAT, p2i(class_space_rs.base()));
      class_space_rs.release();
    }
  }
}

static int archive_regions[]  = {MetaspaceShared::mc,
                                 MetaspaceShared::rw,
                                 MetaspaceShared::ro};
static int archive_regions_count  = 3;

MapArchiveResult MetaspaceShared::map_archive(FileMapInfo* mapinfo, char* mapped_base_address, ReservedSpace rs) {
  assert(UseSharedSpaces, "must be runtime");
  if (mapinfo == NULL) {
    return MAP_ARCHIVE_SUCCESS; // The dynamic archive has not been specified. No error has happened -- trivially succeeded.
  }

  mapinfo->set_is_mapped(false);

  if (mapinfo->alignment() != (size_t)os::vm_allocation_granularity()) {
    log_info(cds)("Unable to map CDS archive -- os::vm_allocation_granularity() expected: " SIZE_FORMAT
                  " actual: %d", mapinfo->alignment(), os::vm_allocation_granularity());
    return MAP_ARCHIVE_OTHER_FAILURE;
  }

  MapArchiveResult result =
    mapinfo->map_regions(archive_regions, archive_regions_count, mapped_base_address, rs);

  if (result != MAP_ARCHIVE_SUCCESS) {
    unmap_archive(mapinfo);
    return result;
  }

  if (!mapinfo->validate_shared_path_table()) {
    unmap_archive(mapinfo);
    return MAP_ARCHIVE_OTHER_FAILURE;
  }

  mapinfo->set_is_mapped(true);
  return MAP_ARCHIVE_SUCCESS;
}

void MetaspaceShared::unmap_archive(FileMapInfo* mapinfo) {
  assert(UseSharedSpaces, "must be runtime");
  if (mapinfo != NULL) {
    mapinfo->unmap_regions(archive_regions, archive_regions_count);
    mapinfo->unmap_region(MetaspaceShared::bm);
    mapinfo->set_is_mapped(false);
  }
}

// Read the miscellaneous data from the shared file, and
// serialize it out to its various destinations.

void MetaspaceShared::initialize_shared_spaces() {
  FileMapInfo *static_mapinfo = FileMapInfo::current_info();
  _i2i_entry_code_buffers = static_mapinfo->i2i_entry_code_buffers();

  // Verify various attributes of the archive, plus initialize the
  // shared string/symbol tables
  char* buffer = static_mapinfo->serialized_data();
  intptr_t* array = (intptr_t*)buffer;
  ReadClosure rc(&array);
  serialize(&rc);

  // Initialize the run-time symbol table.
  SymbolTable::create_table();

  static_mapinfo->patch_archived_heap_embedded_pointers();

  // Close the mapinfo file
  static_mapinfo->close();

  static_mapinfo->unmap_region(MetaspaceShared::bm);

  FileMapInfo *dynamic_mapinfo = FileMapInfo::dynamic_info();
  if (dynamic_mapinfo != NULL) {
    intptr_t* buffer = (intptr_t*)dynamic_mapinfo->serialized_data();
    ReadClosure rc(&buffer);
    SymbolTable::serialize_shared_table_header(&rc, false);
    SystemDictionaryShared::serialize_dictionary_headers(&rc, false);
    dynamic_mapinfo->close();
    dynamic_mapinfo->unmap_region(MetaspaceShared::bm);
  }

  if (PrintSharedArchiveAndExit) {
    if (PrintSharedDictionary) {
      tty->print_cr("\nShared classes:\n");
      SystemDictionaryShared::print_on(tty);
    }
    if (FileMapInfo::current_info() == NULL || _archive_loading_failed) {
      tty->print_cr("archive is invalid");
      vm_exit(1);
    } else {
      tty->print_cr("archive is valid");
      vm_exit(0);
    }
  }
}

// JVM/TI RedefineClasses() support:
bool MetaspaceShared::remap_shared_readonly_as_readwrite() {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint");

  if (UseSharedSpaces) {
    // remap the shared readonly space to shared readwrite, private
    FileMapInfo* mapinfo = FileMapInfo::current_info();
    if (!mapinfo->remap_shared_readonly_as_readwrite()) {
      return false;
    }
    if (FileMapInfo::dynamic_info() != NULL) {
      mapinfo = FileMapInfo::dynamic_info();
      if (!mapinfo->remap_shared_readonly_as_readwrite()) {
        return false;
      }
    }
    _remapped_readwrite = true;
  }
  return true;
}

bool MetaspaceShared::use_full_module_graph() {
#if INCLUDE_CDS_JAVA_HEAP
  if (ClassLoaderDataShared::is_full_module_graph_loaded()) {
    return true;
  }
#endif
  bool result = _use_optimized_module_handling && _use_full_module_graph &&
    (UseSharedSpaces || DumpSharedSpaces) && HeapShared::is_heap_object_archiving_allowed();
  if (result && UseSharedSpaces) {
    // Classes used by the archived full module graph are loaded in JVMTI early phase.
    assert(!(JvmtiExport::should_post_class_file_load_hook() && JvmtiExport::has_early_class_hook_env()),
           "CDS should be disabled if early class hooks are enabled");
  }
  return result;
}

void MetaspaceShared::print_on(outputStream* st) {
  if (UseSharedSpaces) {
    st->print("CDS archive(s) mapped at: ");
    address base = (address)MetaspaceObj::shared_metaspace_base();
    address static_top = (address)_shared_metaspace_static_top;
    address top = (address)MetaspaceObj::shared_metaspace_top();
    st->print("[" PTR_FORMAT "-" PTR_FORMAT "-" PTR_FORMAT "), ", p2i(base), p2i(static_top), p2i(top));
    st->print("size " SIZE_FORMAT ", ", top - base);
    st->print("SharedBaseAddress: " PTR_FORMAT ", ArchiveRelocationMode: %d.", SharedBaseAddress, (int)ArchiveRelocationMode);
  } else {
    st->print("CDS archive(s) not mapped");
  }
  st->cr();
}
