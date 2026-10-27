# KITES : RISC - V SIMULATOR

Kites is a RISC-V simulator and assembly code editor for the RV64 ISA.

| Extension | Single-cycle | 5-stage pipelines |
|-----------|--------------|-------------------|
| I, M, F, D | yes | yes |
| Zicsr (CSR instructions) | yes | yes |
| V (vectors, RVV 1.0), Zve64*/Zve32* | yes | yes |
| Zvfh, Zvbb, Zvkb, Zvbc | yes | yes |

See [docs/VECTOR.md](docs/VECTOR.md) for the vector extension, and
[docs/vector_extension/](docs/vector_extension) for its scope, design and
validation documents.

## Downloading and Installation

Prebuilt libraries are available for Linux,Windows on [Release Page](https://github.com/hghit-harshit/Kites/releases)

### Ubuntu(for only ubuntu-22.04 and above)
- Run chmod +x for the Kites AppImage.
- Run the file.

### Windows
- All the required files are inside the zip file, so you can just run the file.
- In case you get popup regarding some missing dll please contact us.

## MacOS
- Put Kites in the application 

RUN

```bash
sudo xattr -rd com.apple.quarantine sudo xattr -rd com.apple.quarantine /path/to/application.app

```

## Building

To build Kites you will first neet to install Qt(version above 6.9)
Then simply build using

```bash
cmake -DCMAKE_PREFIX_PATH="path/to/Qt/6.x.x/<compiler>" ..
```

## Testing

See [docs/TESTING.md](docs/TESTING.md) for how to build and run tests, including coverage reports.

## CLI

See [docs/CLI.md](docs/CLI.md) for how to build and use the headless command-line executable.
