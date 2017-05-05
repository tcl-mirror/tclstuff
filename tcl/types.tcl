# Types
# ---------------
# Basic types: int, string. Unknown identifiers treated as basic types.
# Name: /likethis (PDF/PostScript convention)
# Boolean: int (0 or 1)
# Currency: int (amount in cents)
# Time: int (seconds since 1970, UTC)
# Date: string (YYYY/MM/DD)
# Vector clock: string (1:2,3:4,5:6)
# Dictionary: e.g., {a:int, b:string}. Unknown fields are ignored.
# Object reference: &table
# Enum: %priority (transported as string, stored as int)
# List: [int], [time, int, string] (striped, length must be multiple of shape)
# Tuple: (date, string) (transport format identical to list)
# Variant dictionary: {case name:type of val1 -> (fields...) val2 -> (fields...) ...}
# Set: e.g., set(int). String equality.
# optlist: e.g., optlist(int), data can be either int or list of ints, result promoted
# to list of ints.
# ifnull: e.g., ifnull(v1, v2), same type as v1. When constructing value of type
# ifnull(v1, v2) result is v1 if v1 is not null, else v2.

namespace eval types {
  variable punct_RE {->|>>|[{}()|&,:*+?\[\]/$@%=-]}
  variable ident_RE {[a-zA-Z_]\w*}
  variable number_RE {0|-?[1-9][0-9]*}
  variable quoted_RE {'[^']*'|\"[^\"]*\"|<[^>]*>}
  variable token_RE "^(?:$punct_RE|$ident_RE|$number_RE|$quoted_RE)"
}

proc types::compile {d} {
  return [parse::parse_string types::parse $d]
}

proc types::get_token {in_var} {
  variable token_RE
  variable ident_RE
  upvar 1 $in_var in

  set tok [parse::rex $token_RE token in]

  # Compiled to jump table. Here is an interesting fact: using
  # backslash rather than "" or {} to escape a case label will cause
  # jump table compilation to fail. Unfortunately, though, Emacs
  # doesn't like {"}. No idea why Tcl won't \-escape simple words in
  # compilation.
  switch [set c [string index $tok 0]] {
    [ {return lbracket} ] {return rbracket}
    "{" {return lbrace} "}" {return rbrace}
    ( {return lparen} ) {return rparen}
    = - + - , - : - * - > - | {return [list $tok]}
    "-" {
      if {$tok eq "-" || $tok eq "->"} {return [list $tok]}
      return [list literal [new_obj int $tok]]
    }
    ' - {"} {# " Work around Emacs's confusion
      return [list literal [new_obj string [string range $tok 1 end-1]]]
    }
    < {return [list literal [new_obj enum [string range $tok 1 end-1]]]}
    / {return [list literal [new_obj name [parse::rex {^[\w-]+} identifier in]]]}
    % {return [list enum [parse::rex {^[a-zA-Z_]\w*} identifier in]]}
    & {return [list ref [parse::rex {^[a-zA-Z_]\w*} identifier in]]}
    $ - @ {
      if {[cursor index $in] eq "\{"} {
        set kind [tree get {$ type_eval @ val_eval} $tok]
        return [list $kind [parse::brace_string in]]
      } else {
        set v [parse_var_name [cursor string $in] [expr {[cursor pos $in]-1}]]
        cursor incr in [expr {[string length $v]-1}]
        return [list [tree get {$ type_var @ val_var} $c] [string range $v 1 end]]
      }
    }
    default {
      if {[regexp {^[a-zA-Z_]} $tok]} {return [list ident $tok]}
      if {[regexp {^[0-9]} $tok]} {return [list literal [new_obj int $tok]]}
      parse::fail "first char of parsed token not handled: '$c'" in
    }
  }
}

proc types::is_const {t} {
  switch [lindex $t 0] {
    literal - val_var - val_eval {return 1}
    tuple - dict {return [expr {[lindex $t 2] == 0}]}
    tree_merge {return [lindex $t 3]}
    default {return 0}
  }
}

proc types::const_val {t {lvl ""}} {
  set lvl [lvl_up $lvl]
  set t [simplify $t $lvl]
  switch [lindex $t 0] {
    literal {return [lindex $t 1]}
    tuple {return [new_obj list [lmap x [lindex $t 1] {const_val $x $lvl}]]}
    dict {
      set d {}
      tree for {k v} [lindex $t 1] {tree set d $k [const_val $v $lvl]}
      return [new_obj dict $d]
    }
    default {error "$t is not a value"}
  }
}

proc types::simplify_h {} {
  return {{
    val_var {
      set val [uplevel $lvl [list set $name]]
      return [list literal [tagged::tag* $val $var_type $lvl]]
    }
    val_eval {
      set val [uplevel $lvl $script]
      return [list literal [tagged::tag* $val $eval_type $lvl]]
    }
    tree_merge {
      if {$is_const} {
        set l [tagged::untag [types::const_val $left $lvl] {{}}]
        set r [tagged::untag [types::const_val $right $lvl] {{}}]
        return [list literal [new_obj dict [tree merge $l $r]]]
      }
      return $_value
    }
    type_var {
      return [uplevel $lvl [list set $name]]
    }
    type_eval {
      return [uplevel $lvl $script]
    }
  }}
}

proc types::simplify {t {lvl ""}} {
  adt map $t [simplify_h] lvl [lvl_up $lvl]
}

proc types::eval {t} {const_val [memo compile $t] "#[expr {[info level]-1}]"}

proc types::parse_variant {in_var} {
  upvar 1 $in_var in
  lassign [parse::exp {~(ident ,':' types::parse ,'of')} in] switch_name switch_type

  set ok {int string enum boolean name}
  if {$switch_type ni $ok} {parse::err "bad switch type $switch_type, must be [oxford $ok]" in}

  set cases {}
  while 1 {
    set x [parse::exp {~(^[rep1sep literal '|'] '->')?} in]
    if {[llength $x] == 0} break
    set labels [lmap l [lindex $x 0] {
      if {[type $l] ne $switch_type} {
        parse::err "case label type ([type $label]) doesn't match switch ($switch_type)" in
      }
      if {[tree exists $cases [val $l]]} {parse::err "duplicate case label [val $l]" in}
      val $l
    }]
    set d {}
    foreach {name type} [parse::exp {[paren [fcomma_sep ~(ident ,':' types::parse)]]} in] {
      if {$name eq $switch_name || [tree exists $d $name]} {err "duplicated field $name" in}
      tree set d $name $type
    }
    foreach l $labels {tree set cases $l $d}
  }
  return [list variant $switch_name $switch_type $cases]
}

proc types::parse_primary {in_var} {
  upvar 1 $in_var in
  parse::exp {
    [bracket [comma_sep types::parse]] >> {
      if {[llength $x] > 0} {list list $x} else {return {list any}}
    }
    | [paren [comma_sep types::parse]] >> {
      set var 0
      foreach v $x {if {![types::is_const $v]} {incr var}}
      list tuple $x $var
    }
    | [brace (~('case' ^[commit types::parse_variant]) | ([fcomma_sep ~(ident ,':' types::parse)] >> {
      set var 0
      set d {}
      foreach {field_name field_type} $x {
        if {[tree exists $d $field_name]} {err "field name $field_name already seen" in}
        if {![types::is_const $field_type]} {incr var}
        tree set d $field_name $field_type
      }
      list dict $d $var
    }))]
    | ~('optlist' ^[commit [paren types::parse]]) >> {
      if {[lindex $x 0] eq "list"} {error "argument to optlist cannot be a list type"}
      list optlist $x
    }
    | ~('ifnull' ^[commit [paren [comma_sep types::parse]]]) >> {
      if {[llength $x] != 2} {error "usage: ifnull(type, value)"}
      lassign $x t v
      list ifnull $t [types::const_val $v]
    }
    | 'try' [paren [comma_sep types::parse]]
    | 'true' >> {list literal [new_obj boolean 1]}
    | 'false' >> {list literal [new_obj boolean 0]}
    | 'this' >> {list literal [new_obj this ""]}
    | 'null' >> {list literal [new_obj null ""]}
    | ('set'|'nullable') [commit [paren types::parse]]
    | ident | $literal | $ref | $enum | $type_var | $type_eval
    | (($val_var | $val_eval) (~('->' ^types::parse) | {return any})) >> {
      lassign $x tok type
      list {*}$tok $type
    }
    | [expected "type"]
  } in
}

proc types::parse {in_var} {
  upvar 1 $in_var in
  set t [parse_primary in]
  foreach postfix [parse::exp {~('|' ^types::parse_primary)*} in] {
    set t [list tree_merge $t $postfix [expr {[is_const $t] && [is_const $postfix]}]]
  }
  return $t
}

proc types::parameterize {} {
  return {{
    val_var {
      return [list literal [tagged::tag* [tree get $val_vars $name] $var_type]]
    }
    type_var {return [tree get $type_vars $name]}
  } ""}
}

adt::define types::adt {
  any        ()
  boolean    ()
  int        ()
  string     ()
  bytearray  ()
  date       ()
  tuple      (types:[*], num_const:int)
  list       (types:[*])
  dict       (fields:{*}, num_const:int)
  literal    (value:_)
  ref        (table:string)
  enum       (name:string)
  eval       (script:string)
  optlist    (base:*)
  ifnull     (base:*, fallback:_)
  set        (elt:*)
  nullable   (base:*)
  bitset     (bitlist:_)
  val_var    (name:string, var_type:*)
  val_eval   (script:string, eval_type:*)
  variant    (switch_name:string, switch_type:*, cases:{{*}})
  tree_merge (left:*, right:*, is_const:int)
  type_var   (name:string)
  type_eval  (script:string)
} ""

proc types::get_compiled {t} {memo compile $t}

