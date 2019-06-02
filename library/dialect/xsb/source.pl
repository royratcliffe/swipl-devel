/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  2019, VU University Amsterdam
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

:- module(xsb_source, []).
:- use_module(library(debug)).
:- use_module(library(lists)).
:- use_module(library(apply)).

/** <module> Support XSB source .P files

This module is a lightweight module that  allows loading .P files as XSB
source files. This module is intended  to be loaded from ``~/.swiplrc``,
providing transparent usage of XSB files  with neglectable impact impact
if no XSB sources are used.
*/

:- multifile
    user:prolog_file_type/2,
    user:term_expansion/2.

user:prolog_file_type('P', prolog).

user:term_expansion(begin_of_file, Out) :-
    prolog_load_context(file, File),
    file_name_extension(Path, 'P', File),
    include_options(File, Include),
    compiler_options(COptions),
    append(Include, COptions, Extra),
    xsb_directives(File, Directives),
    directive_exports(Directives, Public, Directives1),
    (   Public == []
    ->  Out = Out1
    ;   file_base_name(Path, Module),
        Out = [ (:- module(Module, Public))
              | Out1
              ]
    ),
    Out1 = [ (:- expects_dialect(xsb)) | Out2 ],
    append(Extra, More, Out2),
    (   nonvar(Module)
    ->  setup_call_cleanup(
            '$set_source_module'(OldM, Module),
            convlist(head_directive(File), Directives1, More0),
            '$set_source_module'(OldM))
    ;   convlist(head_directive(File), Directives1, More0)
    ),
    flatten(More0, More),
    debug(xsb(header), '~p: directives: ~p', [File, More]).

include_options(File, Option) :-
    (   xsb_header_file(File, FileH)
    ->  Option = [(:- include(FileH))]
    ;   Option = []
    ).

:- multifile xsb:xsb_compiler_option/1.
:- dynamic   xsb:xsb_compiler_option/1.

compiler_options(Directives) :-
    findall(D, mapped_xsb_option(D), Directives).

mapped_xsb_option((:- D)) :-
    xsb:xsb_compiler_option(O),
    map_compiler_option(O, D).

map_compiler_option(singleton_warnings_off, style_check(-singleton)).
map_compiler_option(optimize,               set_prolog_flag(optimise, true)).

xsb_header_file(File, FileH) :-
    file_name_extension(Base, _, File),
    file_name_extension(Base, 'H', FileH),
    exists_file(FileH).

%!  directive_exports(+AllDirectives, -Public, -OtherDirectives)

directive_exports(AllDirectives, Exports, RestDirectives) :-
    partition(is_export, AllDirectives, ExportDirectives, RestDirectives),
    phrase(exports(ExportDirectives), Exports).

is_export(export(_)).

exports([]) -->
    [].
exports([export(H)|T]) -->
    export_decl(H),
    exports(T).

export_decl(Var) -->
    { var(Var),
      !,
      instantiation_error(Var)
    }.
export_decl((A,B)) -->
    !,
    export_decl(A),
    export_decl(B).
export_decl(PI) -->
    [PI].

%!  head_directive(+File, +Directive, -PrefixedDirective) is semidet.

head_directive(File, import(from(Preds, From)),
               (:- xsb_import(Preds, From))) :-
    assertz(xsb:moved_directive(File, import(from(Preds, From)))).
head_directive(File, table(Preds as Options), Clauses) :-
    ignored_table_options(Options),
    expand_term((:- table(Preds)), Clauses),
    assertz(xsb:moved_directive(File, table(Preds as Options))).
head_directive(File, table(Preds), Clauses) :-
    expand_term((:- table(Preds)), Clauses),
    assertz(xsb:moved_directive(File, table(Preds))).

ignored_table_options((A,B)) :-
    !,
    ignored_table_options(A),
    ignored_table_options(B).
ignored_table_options(variant) :-
    !.
ignored_table_options(opaque) :-
    !.
ignored_table_options(Option) :-
    print_message(warning, xsb(table_option_ignored(Option))).

%!  xsb_directives(+File, -Directives) is semidet.
%
%   Directives is a list of all directives in File and its header.
%
%   @bug: track :- op/3 declarations to update the syntax.

xsb_directives(File, Directives) :-
    setup_call_cleanup(
        '$push_input_context'(xsb_directives),
        xsb_directives_aux(File, Directives),
        '$pop_input_context').

xsb_directives_aux(File, Directives) :-
    xsb_header_file(File, FileH),
    !,
    setup_call_cleanup(
        open(FileH, read, In),
        findall(D, stream_directive(In, D), Directives, PDirectives),
        close(In)),
    xsb_P_directives(PDirectives).
xsb_directives_aux(_File, Directives) :-
    xsb_P_directives(Directives).

xsb_P_directives(Directives) :-
    prolog_load_context(stream, In),
    setup_call_cleanup(
        stream_property(In, position(Pos)),
        findall(PI, stream_directive(In, PI), Directives),
        set_stream_position(In, Pos)).

stream_directive(In, Directive) :-
    repeat,
        read_term(In, Term,
                  [ syntax_errors(quiet),
                    module(xsb_source)
                  ]),
        (   Term == end_of_file
        ->  !, fail
        ;   Term = (:- Directive),
            nonvar(Directive)
        ;   fail
        ).

% define the typical XSB operators to limit syntax errors while
% scanning for :- export(_).
:- op(1050,  fy, import).
:- op(1100,  fx, export).
:- op(1100,  fx, mode).
:- op(1040, xfx, from).
:- op(1100,  fy, index).
:- op(1100,  fy, ti).
:- op(1045, xfx, as).
:- op(900,   fy, tnot).
