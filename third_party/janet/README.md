# Janet runtime

Janet 1.41.2 amalgamated source moved unchanged from Visualize's
`external-src/janet/`. Includes the interpreter, public C header, command-line
shell, original license, and source checksums. This is Visualize's vendored
snapshot, including its local runtime fixes; it is not a fresh upstream checkout.

From the repository root:

```sh
make -C third_party/janet
third_party/janet/build/janet -v
third_party/janet/build/janet janet/tests/run.janet
```

A C99 compiler and the platform math/thread libraries are required; Linux also
links libdl. `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, and `BUILD` can be overridden.
The executable is built locally and is not checked into Git.

On the original development machine, Visualize's old directory is a compatibility
symlink here. Its existing ignored `janet` executable remains in place for current
launchers; Visualize can still rebuild it through its own build script.
