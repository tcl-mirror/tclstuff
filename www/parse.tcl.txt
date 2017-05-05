namespace eval parse {
  proc defp {name arg_list body} {
    proc $name [lappend arg_list in_var] "upvar 1 \$in_var in; $body"
  }
  
  defp err {msg} {error [list $in $msg] "" {PARSE ERROR}}
  defp fail {msg} {error [list $in $msg] "" {PARSE FAIL}}
  defp expected {thing} {fail "expected $thing" in}

  defp eof {} {if {![cursor eof $in]} {expected eof in}}
  defp do {p} {{*}$p in}
  defp cat {ps} {lmap p $ps {{*}$p in}}
  defp >> {p script} {set x [{*}$p in]; eval $script}
  defp ret {x} {return $x}
  defp code {script} {eval $script}
  defp = {p val} {prog1 $val {{*}$p in}}
  defp flat {p} {lflatten [{*}$p in]}
  defp app {p prefix} {{*}$prefix [{*}$p in]}
  defp exp {e} {{*}[memo compile_parser $e] in}
  defp guard {p} {set save $in; {*}$p in; set in $save; return}
  defp not {p thing} {
    if {[alt [list [list = $p y] {ret n}] in] eq "y"} {fail "unexpected $thing" in}
  }

  defp seq {ls} {
    set selected 0
    set res {}
    foreach {dir p} $ls {
      set r [{*}$p in]
      switch $dir {
        ign {}
        inl {lappend res {*}$r}
        sel {set selected 1; set sel $r}
        - {lappend res $r}
        default {error "unknown seq directive $dir"}
      }
    }
    if {$selected} {return $sel}
    return $res
  }
    
  defp rep_onto {p ls} {
    set save $in
    try {
      while 1 {lappend ls [{*}$p in]; set save $in}
    } trap {PARSE FAIL} {} {
      set in $save
      return $ls
    }
  }
  defp rep {p} {rep_onto $p {} in}
  defp rep1 {p} {rep_onto $p [list [{*}$p in]] in}

  defp alt {ps} {
    set save $in
    set ok 1
    foreach p $ps {
      try {return [{*}$p in]} trap {PARSE FAIL} r {set ok 0; set in $save}
    }
    if {!$ok} {error $r "" {PARSE FAIL}}; # rethrow last fail
  }
  defp commit {p} {
    try {return [{*}$p in]} trap {PARSE FAIL} {r} {error $r "" {PARSE ERROR}}
  }
  defp opt {p} {alt [list [list app $p list] {ret ""}] in}
  defp rep1sep {p sep} {rep_onto [list seq [list - $sep sel $p]] [list [{*}$p in]] in}
  defp repsep {p sep} {alt [list [list rep1sep $p $sep] {ret ""}] in}
  defp check {p e expect} {set x [{*}$p in]; if $e {return $x}; expected $expect in}
  
  # Note: need to add ^ to beginning of expression unless you want to
  # skip past arbitrary input before the match. Adding it here would
  # prevent compiled regexps from being cached. The parser definition
  # language has the r{...} construct which does automatically add ^.
  defp rex {exp thing} {
    if {[regex::match -cursor -- $exp $in m]} {
      return [prog1 [cursor range {*}$m] {set in [lindex $m 1]}]
    }
    expected $thing in
  }

  defp anychar {} {
    if {[cursor eof $in]} {expected "char" in}
    return [cursor consume in 1]
  }
  defp char {chars} {
    if {[string first [set c [cursor index $in]] $chars] != -1} {return [cursor consume in 1]}
    if {[string length $chars] == 1} {expected $chars in}
    expected "one of \"$chars\"" in
  }

  defp notchar {chars} {
    if {[string first [cursor index $in] $chars] != -1} {expected "not $chars" in}
    anychar in
  }
  
  defp exact {str} {
    if {[cursor consume in [string length $str]] ne $str} {expected \"$str\" in}
    return $str
  }

  defp brace_string {} {
    if {[cursor index $in] ne "\{"} {expected "braced string" in}
    cursor incr in
    return [brace_string_rem in]
  }

  defp brace_string_rem {} {
    set start $in
    set lvl 0
    set bs 0
    while 1 {
      switch [cursor consume in 1] {
        "\{" {if {!$bs} {incr lvl}}
        "\}" {if {!$bs} {if {$lvl} {incr lvl -1} else break}}
        "\\" {set bs 1}
        "" {fail "unfinished brace literal"}
        default {set bs 0}
      }
    }
    return [cursor range $start [cursor move $in -1]]
  }

  # The remaining parsers borrow the tokenizer from the types
  # language.
  defp ws {} {rex {^(?:\s+|/\*(?:[^*]|\*+[^*/])*\*+/)*} "" in}
  defp tok {tok} {
    ws in
    alt [list [list apply {{tok in_var} {
      upvar 1 $in_var in
      prog1 [set t [types::get_token in]] {if {[lindex $t 0] ne $tok} {fail "" in}}
    } ::parse} $tok] [list expected $tok]] in
  }
  defp tok_val {tok} {lindex [tok $tok in] 1}
  defp ident {} {tok_val ident in}
  defp literal {} {tok_val literal in}
  defp keyword {k} {if {[ident in] eq $k} {return $k} else {expected $k in}}
  defp str {} {
    set v [literal in]
    if {[type $v] ne "string"} {expected string in}
    return [val $v]
  }
  defp comma_sep {p} {repsep $p {tok ,} in}
  defp fcomma_sep {p} {lflatten [comma_sep $p in]}
  defp paren {p} {seq [list - {tok lparen} sel $p - {tok rparen}] in}
  defp brace {p} {seq [list - {tok lbrace} sel $p - {tok rbrace}] in}
  defp bracket {p} {seq [list - {tok lbracket} sel $p - {tok rbracket}] in}
}

# Use the combinators defined so far to bootstrap a more pleasant
# language for writing parsers.
proc parse::compile_parser {str} {
  set in [list $str 0]
  return [top {seq {sel alt_expr - ws - eof} in}]
}

proc parse::alt_expr {in_var} {
  upvar 1 $in_var in
  set ls [rep1sep cat_expr {cat {ws {char "|"}}} in]
  if {[llength $ls] == 1} {return [lindex $ls 0]}
  return [list alt $ls]
}

proc parse::cat_expr {in_var} {
  upvar 1 $in_var in
  set ls [rep postfix_expr in]
  if {[llength $ls] == 1} {return [lindex $ls 0]}
  return [list cat $ls]
}

proc parse::postfix_expr {in_var} {
  upvar 1 $in_var in

  # Parse atomic expression
  ws in
  switch [cursor index $in] {
    $ { # named tokens
      cursor incr in; set expr [list tok [rex {^[a-zA-Z_]\w*} "token name" in]]
    } "\[" { # call parser
      set expr [seq {- {char "\["} sel {rep postfix_expr} - ws - {char "\]"}} in]
    } "\{" { # code
      set expr [list code [brace_string in]]
    } "'" { # identifier or symbol token
      set t [str in]
      if {[regex::match {^[a-zA-Z_]} $t]} {
        set expr [list keyword $t]
      } else {
        set expr [list tok $t]
      }
    } "\"" {
      set expr [string range [rex {^\"[^\"]*\"} "string" in] 1 end-1]
    } "(" { # grouping
      set expr [seq {- {char "("} sel alt_expr - ws - {char ")"}} in]
    } "~" { # seq
      cat {{char "~"} ws {char "\("}} in
      set expr [list seq [flat {rep {seq {
        ign ws
        - {>> {alt {{char "@,^"} {ret ""}}} {tree getor {@ inl , ign ^ sel} $x -}}
        - postfix_expr
      }}} in]]
      cat {ws {char "\)"}} in
    } default {
      set expr [alt {
        {seq {- {char "q"} sel brace_string}}
        {>> {seq {- {char "s"} sel brace_string}} {
          if {[string length $x] == 1} {list char $x} else {list exact $x}
        }}
        {>> {seq {- {char "r"} sel brace_string}} {list rex "^(?:$x)" $x}}
        {rex {^[a-zA-Z]\w*(?:::[a-zA-Z]\w*)*} identifier}
      } in]
    }
  }

  # Parse postfixes
  foreach postfix [rep {seq {
    - ws
    sel {alt {
      {= {char "*"} {list rep}}
      {= {char "+"} {list rep1}}
      {= {char "?"} {list opt}}
      {= {exact "{*}"} {apply {{p} {list flat [list rep $p]}}}}
      {= {exact "{+}"} {apply {{p} {list flat [list rep1 $p]}}}}
    }}
  }} in] {
    set expr [{*}$postfix $expr]
  }

  foreach postfix [rep {seq {
    - ws
    sel {alt {
      {>> {seq {- {exact ">>"} - ws sel {commit brace_string}}} {
        list apply {{script p} {list >> $p $script}} $x
      }}
      {>> {seq {- {char "="} - ws sel {commit brace_string}}} {
        list apply {{val p} {list = $p $val}} $x
      }}
    }}
  }} in] {
    set expr [{*}$postfix $expr]
  }

  return $expr
}

# Entry points
proc parse::top {script} {
  try {
    return [uplevel 1 $script]
  } trap {PARSE ERROR} {res d} {
    lassign $res in msg
    error "parse error at \"[cursor string $in]\": [cursor pos $in]: $msg"
  } trap {PARSE FAIL} {res d} {
    lassign $res in msg
    error "parse failed at \"[cursor string $in]\": [cursor pos $in]: $msg"
  }
}

proc parse::parse {parser in_var} {
  upvar 1 $in_var in
  return [top [list exp $parser in]]
#  return [top [list seq [list sel [list exp $parser] - ws - eof] in]]
}

proc parse::parse_string {parser str} {
  set in [list $str 0]
  return [parse $parser in]
}
