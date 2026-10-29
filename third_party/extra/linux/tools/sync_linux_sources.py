#!/usr/bin/env python3
"""Regenerate or validate the checked-in Linux-only patched source mirror."""
from __future__ import annotations
import argparse, hashlib, json, shutil, subprocess, sys, tempfile
from pathlib import Path
def git_blob_sha1(data:bytes)->str:return hashlib.sha1(b"blob "+str(len(data)).encode("ascii")+b"\0"+data).hexdigest()
def sha256(data:bytes)->str:return hashlib.sha256(data).hexdigest()
def fail(message:str)->"NoReturn":
 print(f"source-mirror: ERROR: {message}",file=sys.stderr);raise SystemExit(1)
def find_root(path:Path)->Path:
 for p in path.parents:
  if (p/"runtime"/"frontend_sdl.c").is_file() and (p/"build.mk").is_file():return p
 fail("cannot find repo root; expected runtime/frontend_sdl.c and build.mk")
 raise AssertionError("unreachable")
def main()->int:
 ap=argparse.ArgumentParser(description=__doc__);m=ap.add_mutually_exclusive_group(required=True)
 m.add_argument("--check",action="store_true");m.add_argument("--sync",action="store_true")
 ap.add_argument("--root",type=Path);a=ap.parse_args()
 root=a.root.resolve() if a.root else find_root(Path(__file__).resolve())
 meta=root/"third_party"/"extra"/"linux";mf=meta/"source-mirror-manifest.json"
 if not mf.is_file():fail(f"manifest missing: {mf}")
 manifest=json.loads(mf.read_text(encoding="utf-8"));sources=manifest.get("sources",[]);patches=manifest.get("patches",[])
 if not sources or not patches:fail("manifest must declare sources and ordered patches")
 for s in sources:
  p=root/s["source_path"]
  if not p.is_file():fail(f"canonical source missing: {s['source_path']}")
  actual=git_blob_sha1(p.read_bytes())
  if actual!=s["base_git_blob_sha1"]:fail(f"source drift: {s['source_path']} expected {s['base_git_blob_sha1']}, found {actual}; rebase deliberately")
 with tempfile.TemporaryDirectory(prefix="dqix-linux-source-mirror-") as td:
  stage=Path(td)
  for s in sources:
   dst=stage/s["source_path"];dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/s["source_path"],dst)
  for patch in patches:
   pp=meta/"patches"/patch
   if not pp.is_file():fail(f"patch missing: {patch}")
   result=subprocess.run(["patch","-p1","--batch","--forward","--fuzz=0","--no-backup-if-mismatch","-i",str(pp)],cwd=stage,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
   if result.returncode:fail(f"patch {patch} failed without fuzz:\n{result.stdout}")
   print(result.stdout.rstrip())
  made=[];runtime=b""
  for s in sources:
   data=(stage/s["source_path"]).read_bytes();blob=git_blob_sha1(data);target=root/s["mirror_path"];made.append((s,target,data,blob))
   if s["mirror_path"].endswith("/runtime/frontend_sdl.c"):runtime=data
  for marker in [b"renderer-diagnostics: game_renderer_fallback=",b"renderer-diagnostics: fallback_reason=",b"renderer-diagnostics: gl.vendor=",b"renderer-diagnostics: gl.renderer=",b"renderer-diagnostics: gl.version=",b"renderer-diagnostics: sdl_renderer.name=",b"renderer-diagnostics: sdl_texture.format=",b"dqix_linux_overlay_render_rgba",b"linux_overlay_handle_event",b"dqix_linux_overlay_gl_draw",b'fprintf(f, "discord_app_id = %s\\n"',b"dqix_linux_overlay_fit_rect",b"renderer-diagnostics: OpenGL retry via X11 succeeded"]:
   if marker not in runtime:fail(f"renderer diagnostic marker missing: {marker.decode()}")
  news=[];changed=False
  for s,target,data,blob in made:
   entry=dict(s)
   if a.sync:
    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    if entry.get("mirror_git_blob_sha1")!=blob:entry["mirror_git_blob_sha1"]=blob;changed=True
    print(f"source-mirror: synced {s['source_path']} -> {s['mirror_path']} git-blob={blob} sha256={sha256(data)}")
   else:
    if not target.is_file():fail(f"mirror missing: {s['mirror_path']}; run --sync")
    existing=target.read_bytes()
    if existing!=data or entry.get("mirror_git_blob_sha1")!=blob or git_blob_sha1(existing)!=blob:
     fail(f"mirror mismatch/hash mismatch: {s['mirror_path']} expected {entry.get('mirror_git_blob_sha1')} generated {blob}")
    print(f"source-mirror: OK {s['source_path']} -> {s['mirror_path']} git-blob={blob} sha256={sha256(data)}")
   news.append(entry)
  if a.sync:
   manifest["sources"]=news;mf.write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
   if changed:print(f"source-mirror: updated {mf.relative_to(root)}")
   print("source-mirror: regenerated; review and commit mirrors plus manifest")
  else:print("source-mirror: canonical inputs, patches and mirrors match")
 return 0
if __name__=="__main__":raise SystemExit(main())
