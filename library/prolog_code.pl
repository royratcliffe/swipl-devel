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

:- module(prolog_code,
          [ comma_list/2,                       % (A,B) <-> [A,B]
            semicolon_list/2,                   % (A;B) <-> [A,B]

            mkconj/3,                           % +A, +B, -Conjunction
            mkdisj/3,                           % +A, +B, -Disjunction

            pi_head/2,                          % :PI, :Head
            head_name_arity/3,			% ?Goal, ?Name, ?Arity

            most_general_goal/2                 % :Goal, -General
          ]).
:- use_module(library(error)).

/** <module> Utilities for reasoning about code

This library collects utilities to reason   about  terms commonly needed
for reasoning about Prolog code. Note   that many related facilities can
be found in the core as well as other libraries:

  - =@=/2, subsumes_term/2, etc.
  - library(occurs)
  - library(listing)
  - library(prolog_source)
  - library(prolog_xref)
  - library(prolog_codewalk)
*/

%!  comma_list(?CommaList, ?List).
%!  semicolon_list(?SemicolonList, ?List).
%
%   True if CommaList is a nested term   over  the ','/2 (';'/2) functor
%   and List is a list expressing the   elements of the conjunction. The
%   predicate  is  deterministic  if  at  least  CommaList  or  List  is
%   sufficiently  instantiated.  If  both  are   partial  structures  it
%   enumerates ever growing conjunctions  and   lists.  CommaList may be
%   left or right associative on input. When generated, the CommaList is
%   always right associative.
%
%   This predicate is typically used to reason about Prolog conjunctions
%   (disjunctions) as many operations are easier on lists than on binary
%   trees over some operator.

comma_list(CommaList, List) :-
    phrase(binlist(CommaList, ','), List).
semicolon_list(CommaList, List) :-
    phrase(binlist(CommaList, ';'), List).

binlist(Term, Functor) -->
    { nonvar(Term) },
    !,
    (   { Term =.. [Functor,A,B] }
    ->  binlist(A, Functor),
        binlist(B, Functor)
    ;   [Term]
    ).
binlist(Term, Functor) -->
    [A],
    (   var_tail
    ->  (   { Term = A }
        ;   { Term =.. [Functor,A,B] },
            binlist(B,Functor)
        )
    ;   \+ [_]
    ->  {Term = A}
    ;   binlist(B,Functor),
        {Term =.. [Functor,A,B]}
    ).

var_tail(H, H) :-
    var(H).

%!  mkconj(A,B,Conj) is det.
%!  mkdisj(A,B,Disj) is det.
%
%   Create a conjunction or  disjunction  from   two  terms.  Reduces on
%   `true`.

mkconj(A,B,Conj) :-
    (   is_true(A)
    ->  Conj = B
    ;   is_true(B)
    ->  Conj = A
    ;   Conj = (A,B)
    ).

mkdisj(A,B,Conj) :-
    (   is_false(A)
    ->  Conj = B
    ;   is_false(B)
    ->  Conj = A
    ;   Conj = (A;B)
    ).

is_true(Goal) :- Goal == true.
is_false(Goal) :- (Goal == false -> true ; Goal == fail).

%!  pi_head(?PredicateIndicator, ?Goal) is det.
%
%   Translate between a PredicateIndicator and a   Goal  term. The terms
%   may have a module qualification.
%
%   @error type_error(predicate_indicator, PredicateIndicator)

pi_head(PI, Head) :-
    '$pi_head'(PI, Head).

%!  head_name_arity(?Goal, ?Name, ?Arity) is det.
%
%   Similar to functor/3, but  deals   with  SWI-Prolog's  zero-argument
%   callable terms and avoids creating a   non-callable  term if Name is
%   not an atom and Arity is zero.

head_name_arity(Goal, Name, Arity) :-
    '$head_name_arity'(Goal, Name, Arity).

%!  most_general_goal(+Goal, -General) is det.
%
%   General is the most general version of Goal.  Goal can be qualified.
%
%   @see is_most_general_term/1.

most_general_goal(Goal, General) :-
    var(Goal),
    !,
    General = Goal.
most_general_goal(Goal, General) :-
    atom(Goal),
    !,
    General = Goal.
most_general_goal(M:Goal, M:General) :-
    !,
    most_general_goal(Goal, General).
most_general_goal(Compound, General) :-
    compound_name_arity(Compound, Name, Arity),
    compound_name_arity(General, Name, Arity).


