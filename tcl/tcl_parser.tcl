namespace eval tcl_parser {
  variable whitespace_RE {(?:[\t\v\f\r ]|\\\n)*}
  variable comments_RE [string map [list X $whitespace_RE] {(?:(?:X|\n)*#(?:[^\n\\]|\\.)*)*}]
}

# Required by regex, avoid using [regexp] to mitigate bootstrapping
# issues.
proc tcl_parser::parse_backslash_escape {in_var} {
  upvar 1 $in_var in
  parse::char "\\" in
  set c [cursor index $in]
  foreach v [tree get* {a \x7 b \x8 f \xc n \xa r \xd t \x9 v \xb} $c] {
    cursor incr in; return $v
  }
  foreach digits [tree get* {x 2 u 4 U 8} $c] {
    cursor incr in
    set res 0
    for {set i 0} {$i < $digits} {incr i} {
      if {[string is xdigit [set d [cursor index $in]]]} {
        cursor incr in; set res [expr {$res*16+[scan $d %x]}]
      } else {
        break
      }
    }
    if {$i > 0} {return [format %c $res]} else {return $c}
  }
  if {[cursor eof $in]} {return "\\"}
  return [cursor consume in 1]
}
