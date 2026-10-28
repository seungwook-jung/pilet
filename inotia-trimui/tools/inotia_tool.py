#!/usr/bin/env python3
"""
inotia_tool.py -- APK / native-library inspector and port preparer for the
Inotia (Com2uS) Android games, aimed at the TrimUI Brick port pipeline.

Sub-commands
  inspect  <apk|dir|.so> [--json out.json]
      Report ABIs, ELF class, ARM build attributes (CPU arch, Thumb ISA, FP
      ABI), exported JNI entry points, imports by category, strings that
      name Java classes/JNI signatures, asset inventory, and a verdict on
      which porting route applies (interpreter / native loader / none).
  symbols  <apk|.so> [--grep REGEX] [--out symbols.txt]
      Dump the dynamic symbol table (defined functions + addresses). This is
      the raw material for mods: Com2uS ships un-stripped .dynsym names such
      as SAVE_SaveData, APPINFO_SetTimer, GRP_FillRect, UI_listControl ...
  assets   <apk> [--list] [--extract DIR]
      Inventory or extract assets/ from the APK.
  prepare  <apk> <portdir>
      Put libgame.so (lib/armeabi) and assets.apk (the APK itself) where the
      TrimUI port launcher expects them, after checking the ABI verdict.
  modgen   <apk|.so> --grep REGEX [--out mods.txt]
      Emit a mods.txt skeleton with every matching function listed as a
      commented-out `ret`/`trace` line, to be edited by hand.

Only the Python standard library is required for assets/prepare; `inspect`,
`symbols` and `modgen` need pyelftools (`pip install pyelftools`).

The tool never downloads anything. Supply your own legally obtained APK.
"""
import argparse
import io
import json
import os
import re
import struct
import sys
import zipfile
from collections import Counter, OrderedDict

try:
    from elftools.elf.elffile import ELFFile
    HAVE_ELFTOOLS = True
except Exception:  # pragma: no cover
    HAVE_ELFTOOLS = False

# ----------------------------------------------------------------------------
# Known facts about the Com2uS "wrapper" engine (from the Inotia 3 Vita port's
# analysis of inotia3-1-3-2 lib/armeabi/libgame.so). Used to fingerprint a
# library and to tell which of the known JNI lifecycle entry points it has.
# ----------------------------------------------------------------------------
COM2US_RENDERER_NATIVES = [
    "nativePreInit", "nativeInit", "nativeEvent", "nativeRender",
    "nativeReDraw", "nativePause", "nativeResume", "nativeResumeClet",
    "nativeDone", "nativeCheckQueueEvent",
]
COM2US_JINTERFACE_NATIVES = [
    "nativeDefaultUserInput", "nativeUserInput", "nativeDownloadProcess",
    "nativeForceStopDownload", "nativeWebviewCB", "nativeConnectCB",
]
COM2US_OPTIONAL_CLASSES = [
    "HubConstant", "InApp", "ActiveUser", "WrapperProtection",
    "TapjoyManager", "ADManager", "MopubManager", "NewsBanner",
]
# JNI callbacks the engine makes *into* Java that the interpreter port already
# handles (java.c in the Vita port / jni.c in the wasm port). Anything the .so
# names beyond this list is a candidate for new host work.
KNOWN_JAVA_CALLBACKS = [
    "SetTimer", "UnsetTimer", "Flush", "DrawTextInRect", "setTextViewHeight",
    "setPixel", "GetRealScreenWidth", "GetRealScreenHeight", "HubInitialize",
    "alertPopup", "setGameResolution", "adCreate", "setAdProperty",
    "newsBannerInit", "SoundCreate", "SoundPlay", "SoundStop", "SoundRelease",
    "SoundSetVolume", "SoundIsPlaying", "GetLanguage", "GetCountry",
    "GetDeviceID", "GetMacAddress", "excuteDirectly", "jIsCracked",
    "jGetMemoryHackCheckType",
]

ARM_CPU_ARCH = {
    0: "Pre-v4", 1: "v4", 2: "v4T", 3: "v5T", 4: "v5TE", 5: "v5TEJ", 6: "v6",
    7: "v6KZ", 8: "v6T2", 9: "v6K", 10: "v7", 11: "v6-M", 12: "v6S-M",
    13: "v7E-M", 14: "v8-A", 15: "v8-R", 16: "v8-M.baseline",
    17: "v8-M.mainline", 18: "v8.1-A", 19: "v8.2-A", 20: "v8.3-A",
}
ARM_THUMB_ISA = {0: "none", 1: "Thumb-1", 2: "Thumb-2", 3: "per-CPU-arch"}
ARM_FP_ARCH = {0: "none", 1: "VFPv1", 2: "VFPv2", 3: "VFPv3", 4: "VFPv3-D16",
               5: "VFPv4", 6: "VFPv4-D16", 7: "FP-ARMv8", 8: "FPv5/FP-D16-ARMv8"}
ARM_SIMD = {0: "none", 1: "NEON", 2: "NEONv2", 3: "NEON+ARMv8", 4: "NEON+ARMv8.1"}
ARM_VFP_ARGS = {0: "soft (AAPCS base)", 1: "hard (VFP registers)",
                2: "toolchain-specific", 3: "compatible with both"}

EM_NAMES = {40: "ARM", 183: "AArch64", 3: "x86", 62: "x86_64", 8: "MIPS"}

IMPORT_CATEGORIES = OrderedDict([
    ("gles1_fixed_point", re.compile(r"^gl[A-Z]\w*x(v)?$")),
    ("gles1", re.compile(r"^gl(VertexPointer|ColorPointer|TexCoordPointer|NormalPointer|EnableClientState|DisableClientState|MatrixMode|LoadIdentity|PushMatrix|PopMatrix|Ortho|Frustum|TexEnv|AlphaFunc|ClientActiveTexture|ShadeModel|Color4|LoadMatrix|MultMatrix|Rotate|Scale|Translate|PointSize|LineWidth|Light|Material|Fog|LogicOp)\w*$")),
    ("gles2_shaders", re.compile(r"^gl(CreateShader|ShaderSource|CompileShader|CreateProgram|AttachShader|LinkProgram|UseProgram|GetUniformLocation|Uniform\w+|VertexAttribPointer|EnableVertexAttribArray|GetAttribLocation|BindAttribLocation)$")),
    ("gl_common", re.compile(r"^gl[A-Z]\w+$")),
    ("egl", re.compile(r"^egl[A-Z]\w+$")),
    ("opensl_es", re.compile(r"^(SL_|sl[A-Z]|SLES)")),
    ("android_native", re.compile(r"^(AAsset|AAssetManager|ANativeWindow|ANativeActivity|AInput|AConfiguration|ALooper|__android_log)")),
    ("jni_onload", re.compile(r"^JNI_On(Un)?Load$")),
    ("pthread", re.compile(r"^(pthread_|sem_)")),
    ("sockets", re.compile(r"^(socket|connect|bind|listen|accept|send|sendto|recv|recvfrom|getaddrinfo|freeaddrinfo|gethostbyname|inet_|setsockopt|getsockopt|select|poll|shutdown|getsockname|htons|ntohs|htonl|ntohl)")),
    ("zlib", re.compile(r"^(inflate|deflate|crc32|adler32|compress|uncompress|z_|gz)")),
    ("libm", re.compile(r"^(sin|cos|tan|asin|acos|atan|atan2|sqrt|pow|exp|log|floor|ceil|fmod|ldexp|frexp|modf|fabs|round|lround|isinf|isnan|hypot|cbrt)f?$")),
    ("cxx_runtime", re.compile(r"^(_Z|__cxa_|__gxx_|__aeabi_|_Unwind_|__dynamic_cast|__cxa)")),
    ("dl", re.compile(r"^dl(open|sym|close|error)$")),
    ("libc_io", re.compile(r"^(f?open|close|read|write|lseek|fread|fwrite|fseek|ftell|fclose|fflush|fstat|stat|lstat|mkdir|rmdir|opendir|readdir|closedir|remove|unlink|rename|access|fsync|statfs|fputs|fgets|fputc|fgetc|fprintf|__sF|ftruncate)")),
])


# ----------------------------------------------------------------------------
# Binary AndroidManifest.xml (AXML) -- minimal reader for package/version.
# ----------------------------------------------------------------------------
def parse_axml(data):
    """Return a dict with package, versionCode, versionName, minSdkVersion,
    targetSdkVersion, label-ish info. Best effort; returns {} on failure."""
    out = {}
    try:
        if len(data) < 8 or struct.unpack_from("<I", data, 0)[0] & 0xFFFF != 0x0003:
            return out
        pos = 8
        strings = []
        while pos + 8 <= len(data):
            ctype, csize = struct.unpack_from("<HxxI", data, pos)
            if csize == 0:
                break
            if ctype == 0x0001:  # string pool
                (scount, stcount, flags, soff, stoff) = struct.unpack_from("<IIIII", data, pos + 8)
                utf8 = bool(flags & 0x100)
                offs = struct.unpack_from("<%dI" % scount, data, pos + 28)
                base = pos + soff
                for o in offs:
                    p = base + o
                    try:
                        if utf8:
                            u16len = data[p]
                            p += 2 if u16len & 0x80 else 1
                            blen = data[p]
                            if blen & 0x80:
                                blen = ((blen & 0x7F) << 8) | data[p + 1]
                                p += 2
                            else:
                                p += 1
                            strings.append(data[p:p + blen].decode("utf-8", "replace"))
                        else:
                            slen = struct.unpack_from("<H", data, p)[0]
                            p += 2
                            if slen & 0x8000:
                                slen = ((slen & 0x7FFF) << 16) | struct.unpack_from("<H", data, p)[0]
                                p += 2
                            strings.append(data[p:p + slen * 2].decode("utf-16-le", "replace"))
                    except Exception:
                        strings.append("")
            elif ctype == 0x0102:  # START_TAG
                (line, comment, ns, name, attr_start, attr_size, attr_count) = struct.unpack_from("<IIIIHHH", data, pos + 8)
                tag = strings[name] if 0 <= name < len(strings) else "?"
                ap = pos + 8 + 20 + (attr_start - 20)
                attrs = {}
                for i in range(attr_count):
                    ans, aname, araw, tsize, tres0, ttype, tdata = struct.unpack_from("<IIIHBBI", data, ap + i * attr_size)
                    key = strings[aname] if 0 <= aname < len(strings) else "?"
                    if ttype == 0x03:
                        val = strings[tdata] if 0 <= tdata < len(strings) else ""
                    elif ttype in (0x10, 0x11, 0x12):
                        val = tdata
                    else:
                        val = tdata
                    attrs[key] = val
                if tag == "manifest":
                    for k in ("package", "versionCode", "versionName"):
                        if k in attrs:
                            out[k] = attrs[k]
                elif tag == "uses-sdk":
                    for k in ("minSdkVersion", "targetSdkVersion"):
                        if k in attrs:
                            out[k] = attrs[k]
                elif tag == "application" and "label" in attrs and "label" not in out:
                    out["label"] = attrs["label"]
                elif tag == "activity" and "name" in attrs:
                    out.setdefault("activities", []).append(attrs["name"])
            pos += csize
    except Exception as e:  # pragma: no cover
        out["_error"] = str(e)
    return out


# ----------------------------------------------------------------------------
# ELF analysis
# ----------------------------------------------------------------------------
def arm_attributes(elf):
    """Return {tag: value} from .ARM.attributes, best effort."""
    res = {}
    sec = elf.get_section_by_name(".ARM.attributes")
    if sec is None:
        return res
    try:
        for sub in sec.iter_subsections():
            for subsub in sub.iter_subsubsections():
                for attr in subsub.iter_attributes():
                    res[str(attr.tag)] = attr.value
    except Exception as e:  # pragma: no cover
        res["_error"] = str(e)
    return res


def classify_import(name):
    for cat, rx in IMPORT_CATEGORIES.items():
        if rx.match(name):
            return cat
    return "libc_other"


def analyze_elf(blob, label):
    """Analyze one shared library given as bytes. Returns a dict."""
    r = OrderedDict()
    r["file"] = label
    r["size"] = len(blob)
    if not HAVE_ELFTOOLS:
        r["error"] = "pyelftools not installed (pip install pyelftools)"
        return r
    try:
        elf = ELFFile(io.BytesIO(blob))
    except Exception as e:
        r["error"] = "not an ELF: %s" % e
        return r
    r["elf_class"] = elf.elfclass
    machine = elf["e_machine"]
    if isinstance(machine, int):
        r["machine"] = EM_NAMES.get(machine, str(machine))
    else:
        mm = str(machine).replace("EM_", "")
        r["machine"] = {"ARM": "ARM", "AARCH64": "AArch64", "386": "x86", "X86_64": "x86_64"}.get(mm, mm)
    r["type"] = elf["e_type"]

    # ARM build attributes
    attrs = arm_attributes(elf)
    if attrs:
        a = OrderedDict()
        def pick(tag, table):
            v = attrs.get(tag)
            if v is None:
                return None
            return "%s (%s)" % (table.get(v, "?"), v) if isinstance(v, int) else str(v)
        a["CPU_name"] = attrs.get("TAG_CPU_NAME")
        a["CPU_arch"] = pick("TAG_CPU_ARCH", ARM_CPU_ARCH)
        a["THUMB_ISA_use"] = pick("TAG_THUMB_ISA_USE", ARM_THUMB_ISA)
        a["FP_arch"] = pick("TAG_FP_ARCH", ARM_FP_ARCH)
        a["Advanced_SIMD_arch"] = pick("TAG_ADVANCED_SIMD_ARCH", ARM_SIMD)
        a["ABI_VFP_args"] = pick("TAG_ABI_VFP_ARGS", ARM_VFP_ARGS)
        a["raw"] = {k: (v if isinstance(v, (int, str)) else str(v)) for k, v in attrs.items()}
        r["arm_attributes"] = a

    # Dynamic section flags
    dyn = elf.get_section_by_name(".dynamic")
    flags = []
    needed = []
    soname = None
    if dyn is not None:
        for tag in dyn.iter_tags():
            t = tag.entry.d_tag
            if t == "DT_NEEDED":
                needed.append(tag.needed)
            elif t == "DT_SONAME":
                soname = tag.soname
            elif t == "DT_FLAGS":
                flags.append("DT_FLAGS=0x%x" % tag.entry.d_val)
            elif t == "DT_FLAGS_1":
                flags.append("DT_FLAGS_1=0x%x" % tag.entry.d_val)
            elif t in ("DT_BIND_NOW", "DT_SYMBOLIC", "DT_TEXTREL"):
                flags.append(t)
    r["soname"] = soname
    r["needed"] = needed
    r["dynamic_flags"] = flags

    # Symbols
    dynsym = elf.get_section_by_name(".dynsym")
    exported_funcs = []
    exported_objs = 0
    imports = []
    jni_exports = []
    thumb_funcs = 0
    arm_funcs = 0
    thumb_bytes = 0
    arm_bytes = 0
    if dynsym is not None:
        for sym in dynsym.iter_symbols():
            info = sym["st_info"]
            stype = info["type"]
            shndx = sym["st_shndx"]
            name = sym.name
            if not name:
                continue
            if shndx == "SHN_UNDEF":
                if stype in ("STT_FUNC", "STT_NOTYPE", "STT_OBJECT"):
                    imports.append(name)
                continue
            if stype == "STT_FUNC":
                exported_funcs.append((name, sym["st_value"], sym["st_size"]))
                if name.startswith("Java_"):
                    jni_exports.append(name)
                if r["machine"] == "ARM":
                    if sym["st_value"] & 1:
                        thumb_funcs += 1
                        thumb_bytes += sym["st_size"]
                    else:
                        arm_funcs += 1
                        arm_bytes += sym["st_size"]
            elif stype == "STT_OBJECT":
                exported_objs += 1
    symtab = elf.get_section_by_name(".symtab")
    r["stripped_static_symtab"] = symtab is None
    r["exported_functions"] = len(exported_funcs)
    r["exported_objects"] = exported_objs
    r["jni_exports"] = sorted(jni_exports)
    r["has_JNI_OnLoad"] = any(n == "JNI_OnLoad" for n, _, _ in exported_funcs)
    if r["machine"] == "ARM":
        tot = thumb_bytes + arm_bytes
        r["thumb_share_by_bytes"] = round(100.0 * thumb_bytes / tot, 1) if tot else None
        r["thumb_funcs"] = thumb_funcs
        r["arm_funcs"] = arm_funcs
    cats = Counter(classify_import(n) for n in imports)
    r["imports_total"] = len(imports)
    r["imports_by_category"] = OrderedDict(sorted(cats.items(), key=lambda kv: -kv[1]))
    r["imports"] = sorted(imports)

    # Strings: Java class paths + JNI method signatures + engine markers
    text = blob
    classes = sorted(set(m.decode("ascii", "replace") for m in re.findall(rb"(?:com|net|org|android|java)/[A-Za-z0-9_/$]{4,120}", text)))
    sigs = sorted(set(m.decode("ascii", "replace") for m in re.findall(rb"\((?:\[?[ZBCSIJFD]|\[?L[A-Za-z0-9_/$]+;)*\)(?:\[?[ZBCSIJFDV]|\[?L[A-Za-z0-9_/$]+;)", text)))
    markers = {}
    for key in (b"com2us", b"Com2us", b"gamevil", b"Clet", b"inotia", b"Inotia", b"WrapperRenderer", b"WrapperJinterface", b"excuteDirectly", b"jIsCracked", b"HubInitialize", b"APPINFO", b"MainProcess", b"libgame"):
        c = text.count(key)
        if c:
            markers[key.decode()] = c
    r["java_class_strings"] = classes[:400]
    r["jni_signature_strings"] = sigs[:400]
    r["engine_markers"] = markers

    # Named functions of interest (for the modding side)
    names = [n for n, _, _ in exported_funcs]
    prefixes = Counter()
    for n in names:
        m = re.match(r"^([A-Z][A-Z0-9]{1,12})_", n)
        if m:
            prefixes[m.group(1)] += 1
    r["function_prefix_families"] = OrderedDict(prefixes.most_common(40))

    # Com2uS wrapper fingerprint
    fp = OrderedDict()
    fp["is_com2us_wrapper"] = any("com2us_wrapper" in n for n in jni_exports)
    fp["renderer_natives_present"] = [n for n in COM2US_RENDERER_NATIVES if any(j.endswith("WrapperRenderer_" + n) for j in jni_exports)]
    fp["renderer_natives_missing"] = [n for n in COM2US_RENDERER_NATIVES if n not in fp["renderer_natives_present"]]
    fp["jinterface_natives_present"] = [n for n in COM2US_JINTERFACE_NATIVES if any(j.endswith("WrapperJinterface_" + n) for j in jni_exports)]
    fp["optional_subsystems"] = [c for c in COM2US_OPTIONAL_CLASSES if any(("_" + c + "_") in j for j in jni_exports)]
    r["com2us_fingerprint"] = fp

    # Java callbacks the lib may make (names seen next to signature strings is
    # hard to pair without a decompiler; report candidate method-name strings).
    cb_candidates = sorted(set(m.decode() for m in re.findall(rb"\b(?:Sound[A-Z]\w+|Set[A-Z]\w+|Get[A-Z]\w+|Draw[A-Z]\w+|j[A-Z]\w{3,40}|show[A-Z]\w+|call[A-Z]\w+)\b", text)))
    r["java_callback_name_candidates"] = [c for c in cb_candidates if len(c) < 48][:300]
    r["known_callbacks_seen"] = [c for c in KNOWN_JAVA_CALLBACKS if c.encode() in text]

    r["verdict"] = verdict_for(r)
    return r


def verdict_for(r):
    """Which porting route applies to this library on an aarch64-only Linux
    handheld such as the TrimUI Brick."""
    v = OrderedDict()
    m = r.get("machine")
    a = r.get("arm_attributes") or {}
    if m == "AArch64":
        v["route"] = "native-loader"
        v["detail"] = ("64-bit ARM library: can be loaded natively on the Brick with a "
                       "gmloader-next style ELF loader + bionic/JNI shims (no interpreter). "
                       "That loader frontend does not exist for this engine yet.")
        v["effort"] = "high (new loader frontend; JNI surface must be reimplemented)"
        return v
    if m != "ARM":
        v["route"] = "unsupported"
        v["detail"] = "not an ARM library"
        return v
    arch = str(a.get("CPU_arch") or "")
    thumb = str(a.get("THUMB_ISA_use") or "")
    simd = str(a.get("Advanced_SIMD_arch") or "")
    vfp_args = str(a.get("ABI_VFP_args") or "")
    is_v7 = any(x in arch for x in ("v7", "v8")) or "v6T2" in arch
    v["route"] = "interpreter"
    notes = []
    if is_v7 or "Thumb-2" in thumb:
        notes.append("ARMv7/Thumb-2 code: the Inotia3-wasm interpreter covers ARMv5TE + Thumb-1 "
                     "and only the Thumb-2 encodings Inotia 3 uses; expect decoder work "
                     "(HALT_UNDEFINED traps) before it runs.")
        v["effort"] = "medium-high (extend interpreter: Thumb-2, IT blocks, possibly NEON)"
    else:
        notes.append("ARMv5TE/Thumb-1 soft-float: same ISA profile as Inotia 3 -- the "
                     "interpreter route applies as-is.")
        v["effort"] = "low-medium (engine-specific JNI/patch work only)"
    if simd and "none" not in simd:
        notes.append("NEON present: interpreter has no NEON; those routines need host hooks.")
    if "hard" in vfp_args:
        notes.append("hard-float ABI: argument passing in VFP registers differs from Inotia 3 (soft).")
    fp = r.get("com2us_fingerprint", {})
    if fp.get("is_com2us_wrapper"):
        missing = fp.get("renderer_natives_missing", [])
        notes.append("Com2uS WrapperRenderer lifecycle detected (%d/%d natives)."
                     % (len(fp.get("renderer_natives_present", [])), len(COM2US_RENDERER_NATIVES)))
        if missing:
            notes.append("Lifecycle natives missing vs Inotia 3: %s" % ", ".join(missing))
    else:
        notes.append("No com2us_wrapper JNI exports: a different engine; the Inotia 3 host "
                     "layer (timers, text, sound JNI) will not apply unchanged.")
    cats = r.get("imports_by_category", {})
    if cats.get("gles2_shaders"):
        notes.append("Uses GLES2 shaders: gl4es GLES1 translation is not needed, but the "
                     "host must forward GLES2 (incl. shader sources) instead.")
    if cats.get("opensl_es"):
        notes.append("Uses OpenSL ES directly: needs an OpenSL shim (Inotia 3 used Java SoundManager).")
    if cats.get("android_native"):
        notes.append("Uses Android NDK native APIs (AAsset/ANativeWindow): extra shims required.")
    v["notes"] = notes
    return v


# ----------------------------------------------------------------------------
# Inputs: APK / directory / single .so
# ----------------------------------------------------------------------------
def iter_native_libs(path):
    """Yield (label, bytes) for each lib/<abi>/*.so."""
    if os.path.isfile(path) and zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                if n.startswith("lib/") and n.endswith(".so"):
                    yield n, z.read(n)
    elif os.path.isdir(path):
        for root, _d, files in os.walk(path):
            for f in files:
                if f.endswith(".so"):
                    p = os.path.join(root, f)
                    with open(p, "rb") as fh:
                        yield os.path.relpath(p, path), fh.read()
    elif os.path.isfile(path):
        with open(path, "rb") as fh:
            yield os.path.basename(path), fh.read()


def apk_overview(path):
    o = OrderedDict()
    o["input"] = path
    if not (os.path.isfile(path) and zipfile.is_zipfile(path)):
        return o
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        o["entries"] = len(names)
        o["abis"] = sorted(set(n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2))
        if "AndroidManifest.xml" in names:
            o["manifest"] = parse_axml(z.read("AndroidManifest.xml"))
        o["has_dex"] = any(n.endswith(".dex") for n in names)
        o["has_obb_hint"] = any("obb" in n.lower() for n in names)
        assets = [i for i in z.infolist() if i.filename.startswith("assets/")]
        inv = OrderedDict()
        inv["files"] = len(assets)
        inv["bytes"] = sum(i.file_size for i in assets)
        ext = Counter()
        bytes_by_ext = Counter()
        topdirs = Counter()
        for i in assets:
            e = os.path.splitext(i.filename)[1].lower() or "(none)"
            ext[e] += 1
            bytes_by_ext[e] += i.file_size
            parts = i.filename.split("/")
            if len(parts) > 2:
                topdirs["/".join(parts[:3])] += 1
            else:
                topdirs["assets/"] += 1
        inv["by_extension"] = OrderedDict((k, {"files": v, "bytes": bytes_by_ext[k]}) for k, v in ext.most_common())
        inv["top_dirs"] = OrderedDict(topdirs.most_common(30))
        # sniff container formats on the biggest .dat-ish files
        sniff = []
        for i in sorted(assets, key=lambda i: -i.file_size)[:40]:
            with z.open(i) as fh:
                head = fh.read(16)
            sniff.append({"file": i.filename, "size": i.file_size, "magic": sniff_magic(head)})
        inv["largest_files"] = sniff
        inv["compressed_entries"] = sum(1 for i in assets if i.compress_type != zipfile.ZIP_STORED and not i.is_dir())
        o["assets"] = inv
    return o


def sniff_magic(head):
    if head.startswith(b"\x89PNG"):
        return "png"
    if head.startswith(b"OggS"):
        return "ogg"
    if head.startswith(b"PK\x03\x04"):
        return "zip"
    if head.startswith(b"\x5d\x00\x00"):
        return "lzma-alone"
    if head.startswith(b"\xfd7zXZ"):
        return "xz"
    if head.startswith(b"\x1f\x8b"):
        return "gzip"
    if head.startswith(b"RIFF"):
        return "riff/wav"
    if head.startswith(b"ID3") or head[:2] == b"\xff\xfb":
        return "mp3"
    if head.startswith(b"\x00\x00\x00\x18ftyp") or head[4:8] == b"ftyp":
        return "mp4/m4a"
    if head[:4] == b"dex\n":
        return "dex"
    return "unknown:" + head[:8].hex()


# ----------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------
def cmd_inspect(args):
    report = OrderedDict()
    report["apk"] = apk_overview(args.input)
    libs = []
    for label, blob in iter_native_libs(args.input):
        libs.append(analyze_elf(blob, label))
    report["native_libs"] = libs
    report["recommendation"] = recommend(report)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(report, fh, indent=2, ensure_ascii=False)
    print_summary(report)
    if args.json:
        print("\nfull report: %s" % args.json)
    return 0


def recommend(report):
    libs = report["native_libs"]
    if not libs:
        return {"route": "none", "detail": "no native libraries found (Java-only app, or wrong input)"}
    # Prefer: ARMv5/6 Thumb-1 lib (interpreter as-is) > arm64 (native loader) > armv7
    best = None
    libs = [l for l in libs if "verdict" in l]
    if not libs:
        return {"route": "none", "detail": "native libraries present but none could be parsed as ELF"}
    for l in libs:
        v = l.get("verdict", {})
        score = {"interpreter": 2, "native-loader": 1}.get(v.get("route"), 0)
        if v.get("route") == "interpreter" and "low" in str(v.get("effort", "")):
            score = 3
        if best is None or score > best[0]:
            best = (score, l)
    l = best[1]
    return {"library": l["file"], "route": l["verdict"].get("route"),
            "effort": l["verdict"].get("effort"), "notes": l["verdict"].get("notes", [])}


def print_summary(report):
    a = report["apk"]
    print("== APK ==")
    print("input      :", a.get("input"))
    m = a.get("manifest", {})
    if m:
        print("package    :", m.get("package"), " version:", m.get("versionName"), "(%s)" % m.get("versionCode"),
              " minSdk:", m.get("minSdkVersion"), " targetSdk:", m.get("targetSdkVersion"))
    print("abis       :", ", ".join(a.get("abis", [])) or "-")
    inv = a.get("assets")
    if inv:
        print("assets     : %d files, %.1f MB" % (inv["files"], inv["bytes"] / 1e6))
        print("  by ext   :", ", ".join("%s=%d" % (k, v["files"]) for k, v in list(inv["by_extension"].items())[:12]))
        if inv.get("compressed_entries"):
            print("  WARNING  : %d asset entries are DEFLATE-compressed (repacked APK). The engine needs STORED"
                  " entries; `prepare` rewrites assets.apk accordingly." % inv["compressed_entries"])
    for l in report["native_libs"]:
        print("\n== %s (%d bytes) ==" % (l["file"], l["size"]))
        if "error" in l:
            print("  ", l["error"])
            continue
        print("  ELF%s %s  soname=%s  needed=%s" % (l["elf_class"], l["machine"], l.get("soname"), ",".join(l.get("needed", []))))
        if "arm_attributes" in l:
            at = l["arm_attributes"]
            print("  CPU=%s  Thumb=%s  FP=%s  SIMD=%s  VFP_args=%s" % (
                at.get("CPU_arch"), at.get("THUMB_ISA_use"), at.get("FP_arch"), at.get("Advanced_SIMD_arch"), at.get("ABI_VFP_args")))
            if l.get("thumb_share_by_bytes") is not None:
                print("  Thumb share of named code: %.1f%% (%d thumb / %d arm funcs)" % (l["thumb_share_by_bytes"], l["thumb_funcs"], l["arm_funcs"]))
        print("  exported funcs=%d  objects=%d  JNI exports=%d  JNI_OnLoad=%s  static symtab stripped=%s" % (
            l["exported_functions"], l["exported_objects"], len(l["jni_exports"]), l["has_JNI_OnLoad"], l["stripped_static_symtab"]))
        print("  imports=%d: %s" % (l["imports_total"], ", ".join("%s=%d" % kv for kv in l["imports_by_category"].items())))
        fp = l.get("com2us_fingerprint", {})
        print("  Com2uS wrapper=%s  renderer natives=%d/%d  optional subsystems=%s" % (
            fp.get("is_com2us_wrapper"), len(fp.get("renderer_natives_present", [])), len(COM2US_RENDERER_NATIVES),
            ",".join(fp.get("optional_subsystems", [])) or "-"))
        fam = l.get("function_prefix_families", {})
        if fam:
            print("  function families:", ", ".join("%s(%d)" % kv for kv in list(fam.items())[:16]))
        if l.get("engine_markers"):
            print("  markers:", ", ".join("%s=%d" % kv for kv in l["engine_markers"].items()))
        v = l["verdict"]
        print("  VERDICT: route=%s  effort=%s" % (v.get("route"), v.get("effort")))
        for n in v.get("notes", []):
            print("    -", n)
    rec = report.get("recommendation", {})
    print("\n== RECOMMENDATION ==")
    print("  use %s via %s (%s)" % (rec.get("library"), rec.get("route"), rec.get("effort")))


def load_one_lib(path, prefer_abi=("armeabi", "armeabi-v7a", "arm64-v8a")):
    libs = list(iter_native_libs(path))
    if not libs:
        sys.exit("no native library found in %s" % path)
    for abi in prefer_abi:
        for label, blob in libs:
            if "/%s/" % abi in label or label.endswith(".so") and len(libs) == 1:
                return label, blob
    return libs[0]


def iter_defined_funcs(blob):
    elf = ELFFile(io.BytesIO(blob))
    dynsym = elf.get_section_by_name(".dynsym")
    for sym in dynsym.iter_symbols():
        if sym["st_info"]["type"] == "STT_FUNC" and sym["st_shndx"] != "SHN_UNDEF" and sym.name:
            yield sym.name, sym["st_value"], sym["st_size"]


def cmd_symbols(args):
    if not HAVE_ELFTOOLS:
        sys.exit("pyelftools required: pip install pyelftools")
    label, blob = load_one_lib(args.input)
    rx = re.compile(args.grep) if args.grep else None
    rows = [(n, v, s) for n, v, s in iter_defined_funcs(blob) if not rx or rx.search(n)]
    rows.sort(key=lambda r: r[1])
    out = sys.stdout if not args.out else open(args.out, "w")
    print("# %s: %d function symbols%s" % (label, len(rows), " matching /%s/" % args.grep if args.grep else ""), file=out)
    print("# address(file offset in image, bit0=Thumb)  size  name", file=out)
    for n, v, s in rows:
        print("%08x %6d %s" % (v, s, n), file=out)
    if args.out:
        out.close()
        print("wrote %s (%d symbols)" % (args.out, len(rows)))
    return 0


def cmd_assets(args):
    if not zipfile.is_zipfile(args.apk):
        sys.exit("not a zip/apk: %s" % args.apk)
    with zipfile.ZipFile(args.apk) as z:
        infos = [i for i in z.infolist() if i.filename.startswith("assets/")]
        if args.list or not args.extract:
            for i in infos:
                print("%10d  %s" % (i.file_size, i.filename))
            print("# %d files" % len(infos))
        if args.extract:
            os.makedirs(args.extract, exist_ok=True)
            for i in infos:
                dest = os.path.normpath(os.path.join(args.extract, i.filename))
                if not dest.startswith(os.path.normpath(args.extract)):
                    continue  # zip-slip guard
                if i.is_dir():
                    os.makedirs(dest, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(i) as src, open(dest, "wb") as dst:
                    dst.write(src.read())
            print("extracted %d files to %s" % (len(infos), args.extract))
    return 0


def cmd_prepare(args):
    if not zipfile.is_zipfile(args.apk):
        sys.exit("not a zip/apk: %s" % args.apk)
    os.makedirs(args.portdir, exist_ok=True)
    with zipfile.ZipFile(args.apk) as z:
        names = z.namelist()
        cand = [n for n in names if n.startswith("lib/armeabi/") and n.endswith(".so")]
        if not cand:
            cand = [n for n in names if n.startswith("lib/armeabi-v7a/") and n.endswith(".so")]
            if cand:
                print("WARNING: only armeabi-v7a found; the interpreter route needs decoder work (run `inspect`).")
        if not cand:
            sys.exit("no 32-bit ARM library in the APK; see `inspect` for the arm64 native-loader route.")
        pick = next((n for n in cand if n.endswith("libgame.so")), cand[0])
        blob = z.read(pick)
        if HAVE_ELFTOOLS:
            r = analyze_elf(blob, pick)
            v = r.get("verdict", {})
            print("%s -> route=%s effort=%s" % (pick, v.get("route"), v.get("effort")))
        with open(os.path.join(args.portdir, "libgame.so"), "wb") as fh:
            fh.write(blob)
    # assets.apk: the engine reads resources at byte offsets inside the archive
    # (Android's openFd() model), which only works for STORED entries, and it
    # only ever asks for assets/ -- so the archive written here holds exactly
    # the assets/ entries, stored. No dex, no signature, no resources: nothing
    # the port does not need (and nothing a modded APK's Java side changed).
    out = os.path.join(args.portdir, "assets.apk")
    with zipfile.ZipFile(args.apk) as zin:
        entries = [i for i in zin.infolist() if i.filename.startswith("assets/") and not i.is_dir()]
        deflated = sum(1 for i in entries if i.compress_type != zipfile.ZIP_STORED)
        print("assets.apk: %d asset entries (%d were compressed, now stored)" % (len(entries), deflated))
        with zipfile.ZipFile(out + ".tmp", "w") as zout:
            for i in entries:
                ni = zipfile.ZipInfo(i.filename, date_time=i.date_time)
                ni.external_attr = i.external_attr
                ni.compress_type = zipfile.ZIP_STORED
                zout.writestr(ni, zin.read(i))
        os.replace(out + ".tmp", out)
    print("wrote %s/libgame.so and %s" % (args.portdir, out))
    return 0


def cmd_modgen(args):
    if not HAVE_ELFTOOLS:
        sys.exit("pyelftools required: pip install pyelftools")
    label, blob = load_one_lib(args.input)
    rx = re.compile(args.grep)
    rows = sorted([(n, v, s) for n, v, s in iter_defined_funcs(blob) if rx.search(n)], key=lambda r: r[0])
    out = sys.stdout if not args.out else open(args.out, "w")
    print("# mods.txt skeleton generated from %s for /%s/ (%d functions)" % (label, args.grep, len(rows)), file=out)
    print("# Syntax (one directive per line; '#' starts a comment):", file=out)
    print("#   ret   <symbol> <int>        make the function return a constant immediately", file=out)
    print("#   nop   <symbol>              same as ret <symbol> 0", file=out)
    print("#   poke  <symbol|0xaddr>[+off] u8|u16|u32|f32 <value>   write guest memory after load", file=out)
    print("#   trace <symbol>              log every call with r0-r3 (slow; for reverse engineering)", file=out)
    print("#   fps   <ms>                  engine frame period override (66 = stock 15 fps)", file=out)
    for n, v, s in rows:
        print("# trace %s      # @%08x size %d" % (n, v, s), file=out)
    if args.out:
        out.close()
        print("wrote %s" % args.out)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("inspect"); s.add_argument("input"); s.add_argument("--json"); s.set_defaults(fn=cmd_inspect)
    s = sub.add_parser("symbols"); s.add_argument("input"); s.add_argument("--grep"); s.add_argument("--out"); s.set_defaults(fn=cmd_symbols)
    s = sub.add_parser("assets"); s.add_argument("apk"); s.add_argument("--list", action="store_true"); s.add_argument("--extract"); s.set_defaults(fn=cmd_assets)
    s = sub.add_parser("prepare"); s.add_argument("apk"); s.add_argument("portdir"); s.set_defaults(fn=cmd_prepare)
    s = sub.add_parser("modgen"); s.add_argument("input"); s.add_argument("--grep", required=True); s.add_argument("--out"); s.set_defaults(fn=cmd_modgen)
    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
