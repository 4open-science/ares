# Ares

Ares is an automatic runtime recovery JVM.


## BUILD

1. Install `libhiredis`
2. Install a prebuilt JDK 8, e.g., `jdk1.8.0u65`
3. Check `make/quick_build.sh`

## Known Bugs

1. Currently, we do not clear expression stack before recovering. This is in deed a bug that will crash if GC happens during recovering, particularly when using JPF.
We can copy the expression stack of the top frame into a proper place.
The problem is that this code should be implemented in assembly.
So, dynamical allocation would be non-trivial and introduce performance bottle neck.
A solution is to allocate a fixed buffer per thread as we never do recursive recovering and we can track the max stack in the whole JVM.

A workaround is to use very large heap that avoids GC during JPF.
