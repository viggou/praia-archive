# praia-archive

tar.gz and zip archive support for [Praia](https://github.com/praia-lang/praia). Ships with pre-built native plugins for macOS and Linux.

## Install

```sh
sand install github.com/viggou/praia-archive
```

Pre-built binaries are included for macOS (ARM) and Linux (x86_64).

## Usage

```praia
use "archive"

// Pack a directory into tar.gz or zip
archive.tarPack("mydir", "mydir.tar.gz")
archive.zipPack("mydir", "mydir.zip")

// Unpack
archive.tarUnpack("mydir.tar.gz", "output")
archive.zipUnpack("mydir.zip", "output")

// In-memory: create from content
let data = archive.tarCreate([
    {name: "hello.txt", content: "hello world"},
    {name: "data.json", content: json.stringify({x: 1})}
])
sys.write("archive.tar.gz", data)

// In-memory: create from files on disk
let data = archive.zipCreate([
    {name: "readme.md", path: "./README.md"},
    {name: "src/main.praia", path: "./main.praia"}
])
sys.write("archive.zip", data)

// In-memory: extract
let files = archive.tarExtract(sys.read("archive.tar.gz"))
for (f in files) {
    print(f.name, len(f.content), "bytes")
}
```

## API

| Function | Description |
|----------|-------------|
| `tarCreate(files)` | Create tar.gz from array of `{name, content}` or `{name, path}` |
| `tarExtract(data)` | Extract tar.gz string, returns array of `{name, content}` |
| `tarPack(dir, outPath)` | Pack directory to tar.gz file |
| `tarUnpack(tarPath, outDir)` | Extract tar.gz file to directory |
| `zipCreate(files)` | Create zip from array of `{name, content}` or `{name, path}` |
| `zipExtract(data)` | Extract zip string, returns array of `{name, content}` |
| `zipPack(dir, outPath)` | Pack directory to zip file |
| `zipUnpack(zipPath, outDir)` | Extract zip file to directory |

## Building from source

If you need to rebuild the native plugin (e.g. for a different architecture):

```sh
cd ext_grains/archive
make
```

Requires zlib headers and a C++17 compiler:

```sh
# macOS (pre-installed)
# Ubuntu / Debian
sudo apt install zlib1g-dev
# Fedora / RHEL / Alma
sudo dnf install zlib-devel
```

The Makefile uses `praia --include-path` to find the Praia headers automatically.

## Structure

```
praia-tar/
  grain.yaml
  main.praia          # Praia wrapper, loads native plugin
  plugins/
    archive.cpp       # C++ source
    archive.dylib     # macOS ARM pre-built
    archive.so        # Linux x86_64 pre-built
```
