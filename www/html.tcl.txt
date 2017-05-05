# The routines in this file build/write HTML as well as parse
# it. Users can input HTML using a sexp/SXML-inspired notation:
#
# - (div id=x style={color:red} data-msg="Hi, I'm $name"
#     (b {Child 1}) (i {Child 2}) "Child $n"
#     {*}$more_children)
#
# - (option selected=?$is_sel value=5 {Choose me})
#
# - (p
#     {Character entities such as } &lt; &bull; &gt;
#     { can be parsed as literals, because of course
#       &&<><> <- these get escaped.})
#
# - (span class=$class /* By the way, you can use C-style comments */
#     ${some_cmd arg1 arg2}) /* call out to Tcl to generate a node */
#
# - (ul /* Note: % is aliased to html::construct */
#    {*}${lmap x {1 2 3} {% {(li "Rule $x")}}})
#
# This concrete HTML syntax gets parsed to the following AST (in
# pseudo^3 EBNF notation):
#
# element := "element" *name* [*attr* <value>]
#                      [<element> | <value> | {*}<value>]
# value := "eval" *script*
#        | "subst" *string*
#        | "text_subst" *string*
#        | <node>
# node := "text" *string*
#       | "literal" *string*
#       | "bool" <value>            <- attr only
#       | "atom" *string*           <- attr only
#       | "present"                 <- attr only
#
# Note: "node" in this grammar is short for "non-element node".
#
# Values behave differently in attribute-value and element-child
# contexts (they are really different types that happen to share a
# parser and tag names). For element children, eval and subst give
# nodes or elements. text_subst is treated as text after
# substitution. text is "escaped" using character entities whereas
# literals are inserted directly into generated HTML.
#
# For example, this is how JavaScript can be inserted without
# escaping:
#
# (script type={text/javascript} ${list literal {var x = 1 << 5;}})
#
# In attributes, the results of eval, subst, and text_subst are all
# wrapped in a "text". bool, whose value is expected to be an eval or
# subst, is replaced with either the special value "present" or
# nothing if the substituted value was zero. present values become
# boolean attributes in HTML (for example, <option
# selected>). Attributes with "atom" values are printed without quotes
# (e.g., <div class=x>).
#
# Similar to Tcl, {*} expands a list value inline. The substitued
# value of {*}'s subject is allowed to be either a single node or a
# list of nodes. This works by looking at the zeroth element in the
# list: if that object has llength 1, then it's assumed to be a
# discriminator tag ("element"/"text"/etc.) and the whole list is
# taken as a single value.
#
# This is made possible by the fact that every node is a list of at
# least two elements (other than "present", which syntactically cannot
# occur in this context as it is only generated when processing
# attributes).
#
# The output procedure, html::to_string, works the same way, taking
# either a list of nodes or a single node.

namespace eval html {
  variable entities {
    \x27 &apos;
    \xa0 &nbsp; \xa1 &iexcl; \xa2 &cent; \xa3 &pound; \xa4 &curren;
    \xa5 &yen; \xa6 &brvbar; \xa7 &sect; \xa8 &uml; \xa9 &copy;
    \xaa &ordf; \xab &laquo; \xac &not; \xad &shy; \xae &reg;
    \xaf &macr; \xb0 &deg; \xb1 &plusmn; \xb2 &sup;2 \xb3 &sup;3
    \xb4 &acute; \xb5 &micro; \xb6 &para; \xb7 &middot; \xb8 &cedil;
    \xb9 &sup;1 \xba &ordm; \xbb &raquo; \xbc &frac;14 \xbd &frac;12
    \xbe &frac;34 \xbf &iquest; \xc0 &Agrave; \xc1 &Aacute; \xc2 &Acirc;
    \xc3 &Atilde; \xc4 &Auml; \xc5 &Aring; \xc6 &AElig; \xc7 &Ccedil;
    \xc8 &Egrave; \xc9 &Eacute; \xca &Ecirc; \xcb &Euml; \xcc &Igrave;
    \xcd &Iacute; \xce &Icirc; \xcf &Iuml; \xd0 &ETH; \xd1 &Ntilde;
    \xd2 &Ograve; \xd3 &Oacute; \xd4 &Ocirc; \xd5 &Otilde; \xd6 &Ouml;
    \xd7 &times; \xd8 &Oslash; \xd9 &Ugrave; \xda &Uacute; \xdb &Ucirc;
    \xdc &Uuml; \xdd &Yacute; \xde &THORN; \xdf &szlig; \xe0 &agrave;
    \xe1 &aacute; \xe2 &acirc; \xe3 &atilde; \xe4 &auml; \xe5 &aring;
    \xe6 &aelig; \xe7 &ccedil; \xe8 &egrave; \xe9 &eacute; \xea &ecirc;
    \xeb &euml; \xec &igrave; \xed &iacute; \xee &icirc; \xef &iuml;
    \xf0 &eth; \xf1 &ntilde; \xf2 &ograve; \xf3 &oacute; \xf4 &ocirc;
    \xf5 &otilde; \xf6 &ouml; \xf7 &divide; \xf8 &oslash; \xf9 &ugrave;
    \xfa &uacute; \xfb &ucirc; \xfc &uuml; \xfd &yacute; \xfe &thorn;
    \xff &yuml; \u192 &fnof; \u391 &Alpha; \u392 &Beta; \u393 &Gamma;
    \u394 &Delta; \u395 &Epsilon; \u396 &Zeta; \u397 &Eta; \u398 &Theta;
    \u399 &Iota; \u39A &Kappa; \u39B &Lambda; \u39C &Mu; \u39D &Nu;
    \u39E &Xi; \u39F &Omicron; \u3A0 &Pi; \u3A1 &Rho; \u3A3 &Sigma;
    \u3A4 &Tau; \u3A5 &Upsilon; \u3A6 &Phi; \u3A7 &Chi; \u3A8 &Psi;
    \u3A9 &Omega; \u3B1 &alpha; \u3B2 &beta; \u3B3 &gamma; \u3B4 &delta;
    \u3B5 &epsilon; \u3B6 &zeta; \u3B7 &eta; \u3B8 &theta; \u3B9 &iota;
    \u3BA &kappa; \u3BB &lambda; \u3BC &mu; \u3BD &nu; \u3BE &xi;
    \u3BF &omicron; \u3C0 &pi; \u3C1 &rho; \u3C2 &sigmaf; \u3C3 &sigma;
    \u3C4 &tau; \u3C5 &upsilon; \u3C6 &phi; \u3C7 &chi; \u3C8 &psi;
    \u3C9 &omega; \u3D1 &thetasym; \u3D2 &upsih; \u3D6 &piv;
    \u2022 &bull; \u2026 &hellip; \u2032 &prime; \u2033 &Prime;
    \u203E &oline; \u2044 &frasl; \u2118 &weierp; \u2111 &image;
    \u211C &real; \u2122 &trade; \u2135 &alefsym; \u2190 &larr;
    \u2191 &uarr; \u2192 &rarr; \u2193 &darr; \u2194 &harr; \u21B5 &crarr;
    \u21D0 &lArr; \u21D1 &uArr; \u21D2 &rArr; \u21D3 &dArr; \u21D4 &hArr;
    \u2200 &forall; \u2202 &part; \u2203 &exist; \u2205 &empty;
    \u2207 &nabla; \u2208 &isin; \u2209 &notin; \u220B &ni; \u220F &prod;
    \u2211 &sum; \u2212 &minus; \u2217 &lowast; \u221A &radic;
    \u221D &prop; \u221E &infin; \u2220 &ang; \u2227 &and; \u2228 &or;
    \u2229 &cap; \u222A &cup; \u222B &int; \u2234 &there;4 \u223C &sim;
    \u2245 &cong; \u2248 &asymp; \u2260 &ne; \u2261 &equiv; \u2264 &le;
    \u2265 &ge; \u2282 &sub; \u2283 &sup; \u2284 &nsub; \u2286 &sube;
    \u2287 &supe; \u2295 &oplus; \u2297 &otimes; \u22A5 &perp;
    \u22C5 &sdot; \u2308 &lceil; \u2309 &rceil; \u230A &lfloor;
    \u230B &rfloor; \u2329 &lang; \u232A &rang; \u25CA &loz;
    \u2660 &spades; \u2663 &clubs; \u2665 &hearts; \u2666 &diams;
    \x22 &quot; \x26 &amp; \x3C &lt; \x3E &gt; \u152 &OElig;
    \u153 &oelig; \u160 &Scaron; \u161 &scaron; \u178 &Yuml;
    \u2C6 &circ; \u2DC &tilde; \u2002 &ensp; \u2003 &emsp; \u2009 &thinsp;
    \u200C &zwnj; \u200D &zwj; \u200E &lrm; \u200F &rlm; \u2013 &ndash;
    \u2014 &mdash; \u2018 &lsquo; \u2019 &rsquo; \u201A &sbquo;
    \u201C &ldquo; \u201D &rdquo; \u201E &bdquo; \u2020 &dagger;
    \u2021 &Dagger; \u2030 &permil; \u2039 &lsaquo; \u203A &rsaquo;
    \u20AC &euro;
  }
  
  variable entities_rev [apply {{orig} {
    set d {}
    tree for {char ent} $orig {
      tree set d $ent $char
    }
    return $d
  }} $entities]
}

proc html::parse_attr_list {in_var} {
  upvar 1 $in_var in
  parse::exp {
    ~(,ws r{[\w-]+} ,'=' (html::parse_value |
                          ({return atom} r{[a-zA-Z0-9._:-]+}))){*}
  } in
}

proc html::get_parsed_attr_list {str} {
  return [memo parse::parse_string html::parse_attr_list $str]
}

proc html::parse_element {in_var} {
  upvar 1 $in_var in
  parse::exp {
    [paren [commit ([ret "element"] ident html::parse_attr_list html::parse_element_list)]]
    | html::parse_value
  } in
}

proc html::get_parsed_element_list {str} {
  return [memo parse::parse_string {html::parse_element_list} $str]
}

proc html::parse_value {in_var} {
  upvar 1 $in_var in
  parse::ws in
  switch [cursor index $in] {
    $ {
      if {[cursor index [cursor move $in 1]] eq "\{"} {
        cursor incr in 1
        set str [parse_braces [cursor string $in] [cursor pos $in]]
        cursor incr in [string length $str]
        return [list eval [string range $str 1 end-1]]
      } else {
        set var [parse_var_name [cursor string $in] [cursor pos $in]]
        cursor incr in [string length $var]
        return [list subst $var]
      }
    }
    "\"" - "\{" {
      if {[cursor index $in] eq "\""} {
        set cmd parse_quoted_string; set type text_subst
      } else {
        set cmd parse_braces; set type text
      }
      set str [$cmd [cursor string $in] [cursor pos $in]]
      cursor incr in [string length $str]
      return [list $type [string range $str 1 end-1]]
    }
    ? {
      cursor incr in
      return [list bool [parse_value in]]
    }
    & {
      set esc [parse::rex {^&(?:[a-zA-Z]+|#[0-9]+|#x[0-9a-fA-F]+);} "HTML character entity" in]
      return [list literal $esc]
    }
    default {
      parse::expected "value" in
    }
  }
}

proc html::parse_element_list {in_var} {
  upvar 1 $in_var in
  parse::exp {
    (((ws s{{*}})? html::parse_element) >> {
      lassign $x expand elt
      if {[llength $expand]} {
        if {[lindex $elt 0] ni {subst eval}} {
          parse::err "can't expand [lindex $elt 0]" in
        }
        return [list expand $elt]}
      return $elt
    })*
  } in
}

proc html::compile {str} {
  return [parse::parse_string html::parse_element_list $str]
}

proc html::construct {str} {
  set nodes [memo compile $str]
  set ls [simplify_nodes $nodes "#[expr {[info level]-1}]"]
  if {[llength $nodes] == 1} {return [lindex $ls 0]}
  return $ls
}

# Turn attributes into either text or atom.
proc html::append_attr {var_name attr value level} {
  upvar $var_name t
  switch [lindex $value 0] {
    subst - text_subst {
      set value [list text [uplevel $level [list subst [lindex $value 1]]]]
    }
    eval {
      set value [list text [uplevel $level [lindex $value 1]]]
    }
    bool {
      set d ""
      append_attr d a [lindex $value 1] $level
      if {[llength $d] > 0 && [lindex $d 1 1]} {set value present} else {return}
    }
  }
  tree set t $attr $value
}

proc html::simplify_attrs {attrs level} {
  set res {}
  tree for {attr value} $attrs {
    append_attr res $attr $value $level
  }
  return $res
}

proc html::simplify_nodes {ls level} {
  set res {}
  foreach n $ls {
    append_node $n $level res
  }
  return $res
}

proc html::append_attr_list {var_name attrs {level ""}} {
  upvar 1 $var_name t
  if {$level eq ""} {set level "#[expr {[info level]-1}]"}
  foreach {attr value} [get_parsed_attr_list $attrs] {append_attr t $attr $value $level}
}

proc html::append_node {node level var_name} {
  upvar 1 $var_name ls
  switch [lindex $node 0] {
    element {
      set attrs [simplify_attrs [lindex $node 2] $level]
      set children {}
      foreach c [lindex $node 3] {append_node $c $level children}
      lappend ls [list element [lindex $node 1] $attrs $children]
    }
    text - literal {
      lappend ls $node
    }
    text_subst {
      lappend ls [list text [uplevel $level [list subst [lindex $node 1]]]]
    }
    subst {
      lappend ls [uplevel $level [list subst [lindex $node 1]]]
    }
    eval {
      lappend ls [uplevel $level [lindex $node 1]]
    }
    anon {
      # "anon" elements are resolved in two steps. Here we freeze the
      # uplevel used to evaluate the anon script. Then, route::suspend
      # maps over the whole thing to
      debug "got anon of kind [lindex $node 1]"
      lappend ls [list anonl $level [lindex $node 1] [lindex $node 2]]
    }
    expand {
      set elts {}
      append_node [lindex $node 1] $level elts
      if {[llength [lindex $elts 0 0]] == 1} {
        lappend ls [lindex $elts 0]
      } else {
        lappend ls {*}[lindex $elts 0]
      }
    }
  }
}

proc html::set_attr {var_name attr value} {
  upvar 1 $var_name elt
  assert {[lindex $elt 0] eq "element"}
  lset elt 2 [tree replace [lindex $elt 2] $attr [list text $value]]
}
proc html::elt_name {elt} {
  assert {[lindex $elt 0] eq "element"}
  return [lindex $elt 1]
}
proc html::get_attr {elt attr {def ""}} {
  assert {[lindex $elt 0] eq "element"}
  foreach v [tree get* [lindex $elt 2] $attr] {
    switch [lindex $v 0] {
      atom - text {return [lindex $v 1]}
      present {return $attr}
      default {error "cannot handle attribute: [lindex $v 0]"}
    }
  }
  return $def
}
proc html::child_nodes {elt} {
  assert {[lindex $elt 0] eq "element"}
  return [lindex $elt 3]
}

proc html::to_string {node/s} {
  set res ""
  if {[llength [lindex ${node/s} 0]] == 1} {
    output_node ${node/s} res
  } else {
    foreach n ${node/s} {output_node $n res}
  }
  return $res
}

proc html::output_node {node var_name} {
  upvar 1 $var_name buf
  switch [lindex $node 0] {
    element {
      set elt [lindex $node 1]
      append buf "<[lindex $node 1]"
      tree for {attr value} [lindex $node 2] {
        append buf " $attr"
        switch [lindex $value 0] {
          text {append buf "=\"[escape_text [lindex $value 1]]\""}
          atom {append buf "=[lindex $value 1]"}
          present {}
        }
      }
      append buf ">"
      if {$elt ni {br hr img input link meta}} {
        foreach c [lindex $node 3] {output_node $c buf}
        append buf "</$elt>"
      }
    }
    text {
      append buf [escape_text [lindex $node 1]]
    }
    literal {
      append buf [lindex $node 1]
    }
  }
}

proc html::escape_text {str} {
  variable entities
  set res ""
  set i 0
  while {[set c [string index $str $i]] ne ""} {
    append res [tree getor $entities $c $c]
    incr i
  }
  return $res
}

proc html::unescape_text {str} {
  variable entities_rev
  set ranges [regex::match -indices -all -inline {&(?:[a-zA-Z]+|#[0-9]+|#x[0-9a-fA-F]+);} $str]
  if {[llength $ranges] == 0} {
    return $str; # note: algorithm still works without this optimization
  }
  set res ""
  set cur 0
  foreach r $ranges {
    append res [string range $str $cur [lindex $r 0]-1]
    set esc [string range $str {*}$r]
    if {[string index $esc 1] ne "#"} {
      append res [tree getor $entities_rev $esc $esc]
    } elseif {[string index $esc 2] eq "x"} {
      append res [subst "\\U[string range $esc 3 end-1]"]
    } else {
      append res [subst "\\U[format %x [string range $esc 2 end-1]]"]
    }
    set cur [lindex $r 1]+1
  }
  append res [string range $str $cur end]
  return $res
}

proc html::preformat {text} {
  lmap r [regex::match -all -cursor -inline { +|[^ \n]+|\n} [list $text 0]] {
    set beg [lindex $r 0]
    set end [lindex $r 1]
    switch [cursor index $beg] {
      " " {
        set extra [expr {[cursor pos $end]-[cursor pos $beg]-1}]
        list literal " [string repeat {&nbsp;} $extra]"
      }
      "\n" {% {(br)}}
      default {% {"[cursor range $beg $end]"}}
    }
  }
}

adt::define html::adt {
  element    (name:string, attrs:{*}, children:[*])
  eval       (script:string)
  subst      (val:string)
  text_subst (val:string)
  text       (content:string)
  literal    (val:string)
  bool       (val:*)
  atom       (val:string)
  present    ()
  anon       (kind:string, script:string)
  anonl      (level:string, kind:string, script:string)
}

proc html::map {value fun args} {
  if {[llength [lindex $value 0]] == 1} {
    uplevel 1 [list ::html::adt map $value $fun {*}$args]
  } else {
    lmap x $value {uplevel 1 [list ::html::adt map $x $fun {*}$args]}
  }
}

# Parsing HTML
proc html::parse_token {in_var} {
  upvar 1 $in_var in

  # If input does not start with <, return data up to the next < (or
  # end of input) as "plain".
  switch [cursor index $in] {
    "<" {}
    "" {return eof}
    default {
      set x [cursor find $in <]
      if {[llength $x] == 0} {
        set end [cursor end $in]
      } else {
        set end [lindex $x 0]
      }
      return [list plain [prog1 [list $in $end] {set in $end}]]
    }
  }
  
  # Eat < and whitespace
  parse::rex {^<\s*} "" in

  # Handle everything but start tags.
  switch [cursor index $in] {
    "/" {; # End tag.
      return [list end [parse::exp {~(r{/\s*} ^r{[^\s<>/]+} r{\s*>})} in]]
    }
    "!" {; # SGML declarations, including comments and DOCTYPE.
      return [list declaration [parse::exp {
        ~(s{!} ^~(r{\s*} ^r{[^\s<>/\"=-]+|\"[^\"]*\"|--.*?--})+ r{\s*>})
      } in]]
    }
    "?" {; # XML declaration. Just parse it as soup with no structure.
      return [list xml_declaration \
                  [parse::rex {^\?(?:\s*[^\s<>\"]+|\"[^\"]*\")*\s*\?\s*>} "" in]]
    }
  }

  # Break down start tag.
  lassign [parse::exp {
    r{[^\s<>/]+}
    ~(,r{\s+} r{[^\s<>/=]+} (~(r{\s*=\s*} ^r{\"[^\"]*\"|'[^']*'|[a-zA-Z0-9.-]*}))?){*}
    r{\s*(/?)\s*>}
  } in] elt attrs end

  # Handle self-closing tags and non-HTML data ("script" and "style"
  # elements).
  set kind start
  if {[string first / $end] != -1} {
    set kind start/end
  } elseif {$elt in {script style}} {
    # NOTE: this is correct behavior according to HTML 4.01 but in
    # practice sites embed HTML in string literals inside JS so this
    # doesn't work.
    if {0} {
      if {[regex::match -cursor {</[a-zA-Z]} $in m]} {
        return [prog1 [list foreign $elt $attrs [list $in [lindex $m 0]]] \
                    {set in [lindex $m 0]}]
      }
    }
    
    # Parse until end tag
    foreach end [cursor find $in </$elt] {
      return [prog1 [list foreign $elt $attrs [list $in $end]] \
                  {set in $end}]
    }
    parse::err "$elt does not end" in
  }
  return [list $kind $elt $attrs]
}

# Get values out of attribute lists parsed by parse_token. This is an
# optimization: rather than unquoting/unescaping all attributes, only
# process useful ones.
proc html::get_parsed_attr {attrs name default} {
  foreach {attr val} $attrs {
    if {$attr eq $name} {
      if {[llength $val] == 0} {
        return $attr
      } elseif {[string first [string index [lindex $val 0] 0] "\"'"] != -1} {
        return [unescape_text [string range [lindex $val 0] 1 end-1]]
      } else {
        return [lindex $val 0]
      }
    }
  }
  return $default
}
