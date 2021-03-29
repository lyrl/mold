#include "mold.h"

#include <tbb/global_control.h>
#include <unordered_set>

static std::vector<std::string_view> read_response_file(std::string_view path) {
  std::vector<std::string_view> vec;
  MemoryMappedFile *mb = MemoryMappedFile::must_open(std::string(path));

  auto read_quoted = [&](i64 i, char quote) {
    std::string *buf = new std::string;
    while (i < mb->size() && mb->data()[i] != quote) {
      if (mb->data()[i] == '\\') {
        buf->append(1, mb->data()[i + 1]);
        i += 2;
      } else {
        buf->append(1, mb->data()[i++]);
      }
    }
    if (i >= mb->size())
      Fatal() << path << ": premature end of input";
    vec.push_back(std::string_view(*buf));
    return i + 1;
  };

  auto read_unquoted = [&](i64 i) {
    std::string *buf = new std::string;
    while (i < mb->size() && !isspace(mb->data()[i]))
      buf->append(1, mb->data()[i++]);
    vec.push_back(std::string_view(*buf));
    return i;
  };

  for (i64 i = 0; i < mb->size();) {
    if (isspace(mb->data()[i]))
      i++;
    else if (mb->data()[i] == '\'')
      i = read_quoted(i + 1, '\'');
    else if (mb->data()[i] == '\"')
      i = read_quoted(i + 1, '\"');
    else
      i = read_unquoted(i);
  }
  return vec;
}

std::vector<std::string_view> expand_response_files(char **argv) {
  std::vector<std::string_view> vec;

  for (i64 i = 0; argv[i]; i++) {
    if (argv[i][0] == '@')
      append(vec, read_response_file(argv[i] + 1));
    else
      vec.push_back(argv[i]);
  }
  return vec;
}

static std::vector<std::string> add_dashes(std::string name) {
  // Multi-letter linker options can be preceded by either a single
  // dash or double dashes except ones starting with "o", which must
  // be preceded by double dashes. For example, "-omagic" is
  // interpreted as "-o magic". If you really want to specify the
  // "omagic" option, you have to pass "--omagic".
  if (name[0] == 'o')
    return {"-" + name};
  return {"-" + name, "--" + name};
}

bool read_arg(std::span<std::string_view> &args, std::string_view &arg,
              std::string name) {
  if (name.size() == 1) {
    if (args[0] == "-" + name) {
      if (args.size() == 1)
        Fatal() << "option -" << name << ": argument missing";
      arg = args[1];
      args = args.subspan(2);
      return true;
    }

    if (args[0].starts_with("-" + name)) {
      arg = args[0].substr(name.size() + 1);
      args = args.subspan(1);
      return true;
    }
    return false;
  }

  for (std::string opt : add_dashes(name)) {
    if (args[0] == opt) {
      if (args.size() == 1)
        Fatal() << "option " << name << ": argument missing";
      arg = args[1];
      args = args.subspan(2);
      return true;
    }

    if (args[0].starts_with(opt + "=")) {
      arg = args[0].substr(opt.size() + 1);
      args = args.subspan(1);
      return true;
    }
  }
  return false;
}

bool read_flag(std::span<std::string_view> &args, std::string name) {
  for (std::string opt : add_dashes(name)) {
    if (args[0] == opt) {
      args = args.subspan(1);
      return true;
    }
  }
  return false;
}

static bool read_z_flag(std::span<std::string_view> &args, std::string name) {
  if (args.size() >= 2 && args[0] == "-z" && args[1] == name) {
    args = args.subspan(2);
    return true;
  }

  if (!args.empty() && args[0] == "-z" + name) {
    args = args.subspan(1);
    return true;
  }

  return false;
}

static i64 parse_hex(std::string opt, std::string_view value) {
  if (!value.starts_with("0x") && !value.starts_with("0X"))
    Fatal() << "option -" << opt << ": not a hexadecimal number";
  value = value.substr(2);
  if (value.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
    Fatal() << "option -" << opt << ": not a hexadecimal number";
  return std::stol(std::string(value), nullptr, 16);
}

static i64 parse_number(std::string opt, std::string_view value) {
  if (value.find_first_not_of("0123456789") != std::string_view::npos)
    Fatal() << "option -" << opt << ": not a number";
  return std::stol(std::string(value), nullptr, 16);
}

static std::vector<u8> parse_hex_build_id(std::string_view arg) {
  assert(arg.starts_with("0x") || arg.starts_with("0X"));

  if (arg.size() % 2)
    Fatal() << "invalid build-id: " << arg;
  if (arg.substr(2).find_first_not_of("0123456789abcdefABCDEF") != arg.npos)
    Fatal() << "invalid build-id: " << arg;

  arg = arg.substr(2);

  auto fn = [](char c) {
    if ('0' <= c && c <= '9')
      return c - '0';
    if ('a' <= c && c <= 'f')
      return c - 'a' + 10;
    assert('A' <= c && c <= 'F');
    return c - 'A' + 10;
  };

  std::vector<u8> vec(arg.size() / 2);
  for (i64 i = 0; i < vec.size(); i++)
    vec[i] = (fn(arg[i * 2]) << 4) | fn(arg[i * 2 + 1]);
  return vec;
}

static std::vector<std::string_view>
split(std::string_view str, std::string_view sep) {
  std::vector<std::string_view> vec;

  for (;;) {
    i64 pos = str.find(sep);
    if (pos == str.npos) {
      vec.push_back(str);
      break;
    }
    vec.push_back(str.substr(0, pos));
    str = str.substr(pos);
  }
  return vec;
}

static i64 get_default_thread_count() {
  // mold doesn't scale above 32 threads.
  int n = tbb::global_control::active_value(
    tbb::global_control::max_allowed_parallelism);
  return std::min(n, 32);
}

void parse_nonpositional_args(Context &ctx,
                              std::vector<std::string_view> &remaining) {
  std::span<std::string_view> args = ctx.cmdline_args;
  ctx.arg.thread_count = get_default_thread_count();

  while (!args.empty()) {
    std::string_view arg;

    if (read_flag(args, "v") || read_flag(args, "version")) {
      SyncOut() << "mold " GIT_HASH " (compatible with GNU ld)";
      exit(0);
    }

    if (read_arg(args, arg, "o")) {
      ctx.arg.output = arg;
    } else if (read_arg(args, arg, "dynamic-linker") ||
               read_arg(args, arg, "I")) {
      ctx.arg.dynamic_linker = arg;
    } else if (read_arg(args, arg, "no-dynamic-linker")) {
      ctx.arg.dynamic_linker = "";
    } else if (read_flag(args, "export-dynamic") || read_flag(args, "E")) {
      ctx.arg.export_dynamic = true;
    } else if (read_flag(args, "no-export-dynamic")) {
      ctx.arg.export_dynamic = false;
    } else if (read_flag(args, "Bsymbolic")) {
      ctx.arg.Bsymbolic = true;
    } else if (read_flag(args, "Bsymbolic-functions")) {
      ctx.arg.Bsymbolic_functions = true;
    } else if (read_arg(args, arg, "e") || read_arg(args, arg, "entry")) {
      ctx.arg.entry = arg;
    } else if (read_arg(args, arg, "Map")) {
      ctx.arg.Map = arg;
      ctx.arg.print_map = true;
    } else if (read_flag(args, "print-map") || read_flag(args, "M")) {
      ctx.arg.print_map = true;
    } else if (read_flag(args, "static") || read_flag(args, "Bstatic")) {
      ctx.arg.is_static = true;
      remaining.push_back("-Bstatic");
    } else if (read_flag(args, "Bdynamic")) {
      ctx.arg.is_static = false;
      remaining.push_back("-Bdynamic");
    } else if (read_flag(args, "shared") || read_flag(args, "Bshareable")) {
      ctx.arg.shared = true;
    } else if (read_flag(args, "demangle")) {
      ctx.arg.demangle = true;
    } else if (read_flag(args, "no-demangle")) {
      ctx.arg.demangle = false;
    } else if (read_arg(args, arg, "y") || read_arg(args, arg, "trace-symbol")) {
      ctx.arg.trace_symbol.push_back(arg);
    } else if (read_arg(args, arg, "filler")) {
      ctx.arg.filler = parse_hex("filler", arg);
    } else if (read_arg(args, arg, "L") || read_arg(args, arg, "library-path")) {
      ctx.arg.library_paths.push_back(arg);
    } else if (read_arg(args, arg, "sysroot")) {
      ctx.arg.sysroot = arg;
    } else if (read_arg(args, arg, "u") || read_arg(args, arg, "undefined")) {
      ctx.arg.undefined.push_back(arg);
    } else if (read_arg(args, arg, "init")) {
      ctx.arg.init = arg;
    } else if (read_arg(args, arg, "fini")) {
      ctx.arg.fini = arg;
    } else if (read_arg(args, arg, "hash-style")) {
      if (arg == "sysv") {
        ctx.arg.hash_style_sysv = true;
        ctx.arg.hash_style_gnu = false;
      } else if (arg == "gnu") {
        ctx.arg.hash_style_sysv = false;
        ctx.arg.hash_style_gnu = true;
      } else if (arg == "both") {
        ctx.arg.hash_style_sysv = true;
        ctx.arg.hash_style_gnu = true;
      } else {
        Fatal() << "invalid --hash-style argument: " << arg;
      }
    } else if (read_arg(args, arg, "soname") || read_arg(args, arg, "h")) {
      ctx.arg.soname = arg;
    } else if (read_flag(args, "allow-multiple-definition")) {
      ctx.arg.allow_multiple_definition = true;
    } else if (read_flag(args, "trace")) {
      ctx.arg.trace = true;
    } else if (read_flag(args, "eh-frame-hdr")) {
      ctx.arg.eh_frame_hdr = true;
    } else if (read_flag(args, "no-eh-frame-hdr")) {
      ctx.arg.eh_frame_hdr = false;
    } else if (read_flag(args, "pie") || read_flag(args, "pic-executable")) {
      ctx.arg.pic = true;
      ctx.arg.pie = true;
    } else if (read_flag(args, "no-pie") ||
               read_flag(args, "no-pic-executable")) {
      ctx.arg.pic = false;
      ctx.arg.pie = false;
    } else if (read_flag(args, "relax")) {
      ctx.arg.relax = true;
    } else if (read_flag(args, "no-relax")) {
      ctx.arg.relax = false;
    } else if (read_flag(args, "perf")) {
      ctx.arg.perf = true;
    } else if (read_flag(args, "stats")) {
      ctx.arg.stats = true;
      Counter::enabled = true;
    } else if (read_flag(args, "warn-common")) {
      ctx.arg.warn_common = true;
    } else if (read_flag(args, "no-warn-common")) {
      ctx.arg.warn_common = false;
    } else if (read_z_flag(args, "now")) {
      ctx.arg.z_now = true;
    } else if (read_z_flag(args, "execstack")) {
      ctx.arg.z_execstack = true;
    } else if (read_z_flag(args, "noexecstack")) {
      ctx.arg.z_execstack = false;
    } else if (read_z_flag(args, "relro")) {
      ctx.arg.z_relro = true;
    } else if (read_z_flag(args, "norelro")) {
      ctx.arg.z_relro = false;
    } else if (read_z_flag(args, "defs")) {
      ctx.arg.z_defs = true;
    } else if (read_z_flag(args, "nodefs")) {
      ctx.arg.z_defs = false;
    } else if (read_z_flag(args, "nodlopen")) {
      ctx.arg.z_dlopen = false;
    } else if (read_z_flag(args, "nodelete")) {
      ctx.arg.z_delete = false;
    } else if (read_z_flag(args, "nocopyreloc")) {
      ctx.arg.z_copyreloc = false;
    } else if (read_flag(args, "no-undefined")) {
      ctx.arg.z_defs = true;
    } else if (read_flag(args, "fatal-warnings")) {
      ctx.arg.fatal_warnings = true;
    } else if (read_flag(args, "no-fatal-warnings")) {
      ctx.arg.fatal_warnings = false;
    } else if (read_flag(args, "fork")) {
      ctx.arg.fork = true;
    } else if (read_flag(args, "no-fork")) {
      ctx.arg.fork = false;
    } else if (read_flag(args, "gc-sections")) {
      ctx.arg.gc_sections = true;
    } else if (read_flag(args, "no-gc-sections")) {
      ctx.arg.gc_sections = false;
    } else if (read_flag(args, "print-gc-sections")) {
      ctx.arg.print_gc_sections = true;
    } else if (read_flag(args, "no-print-gc-sections")) {
      ctx.arg.print_gc_sections = false;
    } else if (read_flag(args, "icf")) {
      ctx.arg.icf = true;
    } else if (read_flag(args, "no-icf")) {
      ctx.arg.icf = false;
    } else if (read_flag(args, "quick-exit")) {
      ctx.arg.quick_exit = true;
    } else if (read_flag(args, "no-quick-exit")) {
      ctx.arg.quick_exit = false;
    } else if (read_flag(args, "print-icf-sections")) {
      ctx.arg.print_icf_sections = true;
    } else if (read_flag(args, "no-print-icf-sections")) {
      ctx.arg.print_icf_sections = false;
    } else if (read_flag(args, "quick-exit")) {
      ctx.arg.quick_exit = true;
    } else if (read_flag(args, "no-quick-exit")) {
      ctx.arg.quick_exit = false;
    } else if (read_arg(args, arg, "thread-count")) {
      ctx.arg.thread_count = parse_number("thread-count", arg);
    } else if (read_flag(args, "threads")) {
      ctx.arg.thread_count = get_default_thread_count();
    } else if (read_flag(args, "no-threads")) {
      ctx.arg.thread_count = 1;
    } else if (read_flag(args, "discard-all") || read_flag(args, "x")) {
      ctx.arg.discard_all = true;
    } else if (read_flag(args, "discard-locals") || read_flag(args, "X")) {
      ctx.arg.discard_locals = true;
    } else if (read_flag(args, "strip-all") || read_flag(args, "s")) {
      ctx.arg.strip_all = true;
    } else if (read_flag(args, "strip-debug") || read_flag(args, "S")) {
      ctx.arg.strip_debug = true;
    } else if (read_arg(args, arg, "rpath")) {
      if (!ctx.arg.rpaths.empty())
        ctx.arg.rpaths += ":";
      ctx.arg.rpaths += arg;
    } else if (read_arg(args, arg, "version-script")) {
      parse_version_script(ctx, std::string(arg));
    } else if (read_arg(args, arg, "dynamic-list")) {
      parse_dynamic_list(ctx, std::string(arg));
    } else if (read_flag(args, "build-id")) {
      ctx.arg.build_id.kind = BuildId::HASH;
      ctx.arg.build_id.hash_size = 20;
    } else if (read_arg(args, arg, "build-id")) {
      if (arg == "none") {
        ctx.arg.build_id.kind = BuildId::NONE;
      } else if (arg == "uuid") {
        ctx.arg.build_id.kind = BuildId::UUID;
      } else if (arg == "md5") {
        ctx.arg.build_id.kind = BuildId::HASH;
        ctx.arg.build_id.hash_size = 16;
      } else if (arg == "sha1") {
        ctx.arg.build_id.kind = BuildId::HASH;
        ctx.arg.build_id.hash_size = 20;
      } else if (arg == "sha256") {
        ctx.arg.build_id.kind = BuildId::HASH;
        ctx.arg.build_id.hash_size = 32;
      } else if (arg.starts_with("0x") || arg.starts_with("0X")) {
        ctx.arg.build_id.kind = BuildId::HEX;
        ctx.arg.build_id.value = parse_hex_build_id(arg);
      } else {
        Fatal() << "invalid --build-id argument: " << arg;
      }
    } else if (read_flag(args, "no-build-id")) {
      ctx.arg.build_id.kind = BuildId::NONE;
    } else if (read_arg(args, arg, "auxiliary") || read_arg(args, arg, "f")) {
      ctx.arg.auxiliary.push_back(arg);
    } else if (read_arg(args, arg, "filter") || read_arg(args, arg, "F")) {
      ctx.arg.filter.push_back(arg);
    } else if (read_arg(args, arg, "exclude-libs")) {
      ctx.arg.exclude_libs = split(arg, ",");
    } else if (read_flag(args, "preload")) {
      ctx.arg.preload = true;
    } else if (read_arg(args, arg, "z")) {
    } else if (read_arg(args, arg, "O")) {
    } else if (read_flag(args, "O0")) {
    } else if (read_flag(args, "O1")) {
    } else if (read_flag(args, "O2")) {
    } else if (read_arg(args, arg, "plugin")) {
    } else if (read_arg(args, arg, "plugin-opt")) {
    } else if (read_flag(args, "color-diagnostics")) {
    } else if (read_flag(args, "gdb-index")) {
    } else if (read_arg(args, arg, "m")) {
    } else if (read_flag(args, "eh-frame-hdr")) {
    } else if (read_flag(args, "start-group")) {
    } else if (read_flag(args, "end-group")) {
    } else if (read_flag(args, "(")) {
    } else if (read_flag(args, ")")) {
    } else if (read_flag(args, "fatal-warnings")) {
    } else if (read_flag(args, "enable-new-dtags")) {
    } else if (read_flag(args, "disable-new-dtags")) {
    } else if (read_arg(args, arg, "sort-section")) {
    } else if (read_flag(args, "sort-common")) {
    } else if (read_arg(args, arg, "rpath-link")) {
    } else if (read_flag(args, "as-needed")) {
      remaining.push_back("-as-needed");
    } else if (read_flag(args, "no-as-needed")) {
      remaining.push_back("-no-as-needed");
    } else if (read_flag(args, "whole-archive")) {
      remaining.push_back("-whole-archive");
    } else if (read_flag(args, "no-whole-archive")) {
      remaining.push_back("-no-whole-archive");
    } else if (read_arg(args, arg, "l")) {
      remaining.push_back("-l");
      remaining.push_back(arg);
    } else if (read_arg(args, arg, "script") || read_arg(args, arg, "T")) {
      remaining.push_back(arg);
    } else if (read_flag(args, "push-state")) {
      remaining.push_back("-push-state");
    } else if (read_flag(args, "pop-state")) {
      remaining.push_back("-pop-state");
    } else {
      if (args[0][0] == '-')
        Fatal() << "mold: unknown command line option: " << args[0];
      remaining.push_back(args[0]);
      args = args.subspan(1);
    }
  }

  if (ctx.arg.shared) {
    ctx.arg.pic = true;
    ctx.arg.dynamic_linker = "";
  }

  if (ctx.arg.pic)
    ctx.arg.image_base = 0;

  if (!ctx.arg.shared) {
    if (!ctx.arg.filter.empty())
      Fatal() << "-filter may not be used without -shared";
    if (!ctx.arg.auxiliary.empty())
      Fatal() << "-auxiliary may not be used without -shared";
  }

  if (ctx.arg.output.empty())
    ctx.arg.output = "a.out";
}
