; Test that -codeview-emit-nested-anonymous-types keeps the parent struct's
; field list as a single member referencing an LF_UNION record, instead of
; flattening the union's members into the parent.
;
; C source the IR below corresponds to:
;
;   struct S {
;     int before;
;     union {
;       int a;
;       int b;
;     };
;     int after;
;   } gS;

; RUN: llc < %s -filetype=obj -o %t.o
; RUN: llvm-readobj %t.o --codeview | FileCheck %s --check-prefix=FLAT

; RUN: llc < %s -filetype=obj -o %t.o -codeview-emit-nested-anonymous-types
; RUN: llvm-readobj %t.o --codeview | FileCheck %s --check-prefix=NESTED

; Default (flat): the anonymous union's members `a` and `b` are inlined into
; struct S's field list at offset 0x4 — so we see three DataMember entries
; named `before`, `a`, `b`, `after` (a and b at the same offset).
;
; FLAT: FieldList ({{.*}}) {
; FLAT:   TypeLeafKind: LF_FIELDLIST
; FLAT:   DataMember {
; FLAT:     Name: before
; FLAT:   }
; FLAT:   DataMember {
; FLAT:     FieldOffset: 0x4
; FLAT:     Name: a
; FLAT:   }
; FLAT:   DataMember {
; FLAT:     FieldOffset: 0x4
; FLAT:     Name: b
; FLAT:   }
; FLAT:   DataMember {
; FLAT:     Name: after
; FLAT:   }
; FLAT: }

; With -codeview-emit-nested-anonymous-types: the union is emitted as its
; own LF_UNION record and the parent struct holds a single unnamed
; DataMember pointing at it at the union's offset.
;
; NESTED: Union ({{.*}}) {
; NESTED:   TypeLeafKind: LF_UNION
; NESTED:   FieldList: <field list>
; NESTED: }
; NESTED: FieldList ({{.*}}) {
; NESTED:   TypeLeafKind: LF_FIELDLIST
; NESTED:   DataMember {
; NESTED:     Name: before
; NESTED:   }
; NESTED:   DataMember {
; NESTED:     FieldOffset: 0x4
; NESTED:     Name:{{ *$}}
; NESTED:   }
; NESTED:   DataMember {
; NESTED:     Name: after
; NESTED:   }
; NESTED: }

source_filename = "t.c"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

%struct.S = type { i32, %union.anon, i32 }
%union.anon = type { i32 }

@gS = dso_local global %struct.S zeroinitializer, align 4, !dbg !0

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!13, !14, !15}
!llvm.ident = !{!16}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "gS", scope: !2, file: !3, line: 9, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C99, file: !3, producer: "clang", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4)
!3 = !DIFile(filename: "t.c", directory: ".")
!4 = !{!0}
!5 = !{}
!6 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "S", file: !3, line: 1, size: 96, elements: !7)
!7 = !{!8, !10, !17}
!8 = !DIDerivedType(tag: DW_TAG_member, name: "before", scope: !6, file: !3, line: 2, baseType: !9, size: 32)
!9 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!10 = !DIDerivedType(tag: DW_TAG_member, scope: !6, file: !3, line: 3, baseType: !11, size: 32, offset: 32)
!11 = distinct !DICompositeType(tag: DW_TAG_union_type, scope: !6, file: !3, line: 3, size: 32, elements: !12)
!12 = !{!18, !19}
!17 = !DIDerivedType(tag: DW_TAG_member, name: "after", scope: !6, file: !3, line: 7, baseType: !9, size: 32, offset: 64)
!18 = !DIDerivedType(tag: DW_TAG_member, name: "a", scope: !11, file: !3, line: 4, baseType: !9, size: 32)
!19 = !DIDerivedType(tag: DW_TAG_member, name: "b", scope: !11, file: !3, line: 5, baseType: !9, size: 32)
!13 = !{i32 2, !"CodeView", i32 1}
!14 = !{i32 2, !"Debug Info Version", i32 3}
!15 = !{i32 1, !"wchar_size", i32 2}
!16 = !{!"clang"}
