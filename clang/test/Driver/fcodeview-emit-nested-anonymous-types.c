// RUN: %clang -### -fcodeview-emit-nested-anonymous-types -c %s 2>&1 | FileCheck %s

// CHECK: "-fcodeview-emit-nested-anonymous-types"
