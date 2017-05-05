proc id {x} {return $x}
proc prog1 {x script} {uplevel 1 $script; return $x}
proc getor {l d} {
  if {[llength $l] == 0} {return $d}
  return [lindex $l 0]
}

proc close_eventually {var_name} {
  upvar 1 $var_name chan
  trace add variable chan unset \
      [list apply {{chan args} {catch {close $chan}}} $chan]
}
proc remove_traces {var_name} {
  upvar 1 $var_name v
  foreach x [trace info variable v] {trace remove variable v {*}$x}
}

proc kill {var_name} {upvar 1 $var_name var; return $var[set var ""]}

# TODO: switch to regex
proc remove_leading_zeros {x} {return [regsub {^0*(?=[0-9]+)} $x ""]}
proc is_int {x} {return [regexp {^(0|-?[1-9][0-9]*)$} $x]}
proc is_nonneg_int {x} {return [regexp {^(0|[1-9][0-9]*)$} $x]}
proc remove_prefix {x p} {
  if {[string range $x 0 [string length $p]-1] eq $p} {
    return [string range $x [string length $p] end]
  }
  return $x
}

proc assert {cond} {
  if {![uplevel 1 [list expr $cond]]} {error "assertion $cond failed"}
}

proc split_at {str boundary} {
  set res {}
  set pos 0
  while {1} {
    if {[set next [string first $boundary $str $pos]] == -1} {
      return [lappend res [string range $str $pos end]]
    }
    lappend res [string range $str $pos $next-1]
    set pos [expr {$next+[string length $boundary]}]
  }
}

# Make a namespace variable with a default value
proc variable_default {name default} {
  set ns [uplevel 1 {namespace current}]
  if {![info exists ${ns}::${name}]} {
    set ${ns}::${name} $default
  }
}

array set id_counters {}
proc gen_id {{prefix "id_"}} {
  global id_counters
  set ns [uplevel 1 {namespace current}]
  if {$ns eq "::"} {set ns ""}; # :::: works, but ugly
  return ${ns}::${prefix}[incr id_counters($ns)]
}

variable_default tmp_ctr 0
proc tmpfile_init {} {
  file delete -force tmp
  file mkdir tmp
  file attributes tmp -permission go-rwx
}
proc tmpfile_data {data} {
  global tmp_ctr
  return [prog1 [set f tmp/[incr tmp_ctr]] {write_file $f $data}]
}
proc tmpfile_chan {{mode wb}} {
  global tmp_ctr
  return [open tmp/[incr tmp_ctr] $mode]
}

proc lvl_up {lvl} {
  if {$lvl eq ""} {return "#[expr {[info level]-2}]"}
  return $lvl
}

# Data where the compressed and plain text are lazily computed from
# each other. SHA1 hash also saved.
proc new_data {args} {
  set r {{plain compressed sha1 decoded} "" "" "" ""}
  set have_plain 0
  set have_compressed 0
  set have_hash 0
  set have_decoded 0
  foreach {type d} $args {
    switch $type {
      -plain {
        : r plain [list id $d]
        : r compressed [list zlib deflate $d]
      }
      -compressed {
        : r plain [list zlib inflate $d]
        : r compressed [list id $d]
      }
      -hash {
        set have_hash 1
        : r sha1 [list id $d]
      }
      -decoded {
        set have_decoded 1
        : r decoded [list id $d]
      }
    }
  }
  if {$have_decoded} {
    if {!$have_plain && !$have_compressed} {
      : r plain [list buencode::encode [lindex [: $r decoded] 1]]
      : r compressed [list apply {p {zlib deflate [force $p]}} [: $r plain]]
    }
  } else {
    : r decoded [list apply {p {buencode::decode [force $p]}} [: $r plain]]
  }
  if {!$have_hash} {
    : r sha1 [list apply {p {sha1 [force $p]}} [: $r plain]]
  }
  return $r
}

proc data_plain {d} {return [force [: $d plain]]}
proc data_compressed {d} {return [force [: $d compressed]]}
proc data_hash {d} {return [force [: $d sha1]]}
proc data_decoded {d} {return [force [: $d decoded]]}

# Recrod properties. Records, as implemented by C, have a field list
# as their first element, followed by the fields. We append an
# additional unnamed "properties" slot at the end as a critbit tree.
proc rec_prop {rec prop {default ""}} {
  set props [lindex $rec [llength [lindex $rec 0]]+1]
  return [tree getor $props $prop $default]
}

proc rec_set_prop {var_name prop value {del_if ""}} {
  upvar 1 $var_name rec
  set fields [lindex $rec 0]
  set prop_index [expr {[llength $fields]+1}]
  set props [lindex $rec $prop_index]
  lset rec $prop_index ""
  if {[llength $del_if] && [apply $del_if $value]} {
    tree unset props $prop
  } else {
    tree set props $prop $value
  }
  lset rec $prop_index $props
}

proc rec_unset_prop {var_name prop} {
  upvar 1 $var_name rec
  rec_set_prop rec $prop "" {x {return 1}}
}

proc rec_mod_prop {rec_name prop var_name body} {
  upvar 1 $rec_name rec
  uplevel 1 [list set $var_name [rec_prop $rec $prop]]
  rec_set_prop rec $prop [uplevel 1 $body]
}
              
# [-> $d #key] => [dict get $d key]
# [-> $l .n] => [lindex $l n]
proc -> {value args} {
  for {set i 0} {$i < [llength $args]} {incr i} {
    set mod [lindex $args $i]
    switch -glob -- $mod {
      ".*" {set value [lindex $value [string range $mod 1 end]]}
      "#*" {set value [tree get $value [string range $mod 1 end]]}
      "()" {set value [{*}[lindex $args [incr i]] $value]}
      default {error "unknown modifier $mod"}
    }
  }
  return $value
}

proc closure {argsList body args} {
  set lifts {}
  set new {}
  set vals {}
  foreach v $argsList {
    if {[string index $v 0] eq "^"} {
      set v [string range $v 1 end]
      lappend lifts $v
      lappend vals [uplevel 1 [list set $v]]
    } else {
      lappend new $v
    }
  }
  
  # Be careful to put new vars after lifts because of the case of a final
  # new var "args"; can't move it to the middle.
  list apply [list [concat $lifts $new] $body] {*}$vals {*}$args
}

proc slurp_file {filename} {
  return [prog1 [read [set f [open $filename rb]]] {close $f}]
}
proc write_file {filename content} {
  puts -nonewline [set f [open $filename wb]] $content
  close $f
}

# Parsing context
proc s_init {str {i 0}} {return [list {str pos} $str $i]}
proc s_eof {in} {return [expr {[: $in pos] >= [string length [: $in str]]}]}
proc s_cur {in {i 0}} {return [string index [: $in str] [: $in pos]+$i]}
proc s_rem {in} {return [string range [: $in str] [: $in pos] end]}

proc s_adv {in_var {incr 1}} {
  upvar 1 $in_var in
  set old [: $in pos]
  : in pos [expr {$old + $incr}]
  return [string range [: $in str] $old [expr {$old + $incr - 1}]]
}

# Warning: doesn't do CRLF. This is for the fossil protocol only.
proc s_gets {in_var} {
  upvar 1 $in_var in
  set str [: $in str]
  
  # Find index of "\n" without using [string first] so that we don't
  # stringify a byte array.
  for {set i [: $in pos]} {$i < [string length $str]} {incr i} {
    if {[string index $str $i] eq "\n"} {
      set line [string range $str [: $in pos] [expr {$i-1}]]
      : in pos [expr {$i+1}]
      return $line
    }
  }

  set line [string range $str [: $in pos] end]
  : in pos [string length $str]
  return $line
}

proc debug {msg} {
  set ns [regsub {^::} [uplevel 1 {namespace current}] ""]
  puts "DEBUG $ns: $msg"
  flush stdout
}

proc lpop {ls_var} {
  upvar 1 $ls_var ls
  set r [lindex $ls end]
  set ls [lreplace [kill ls] end end]
  return $r
}

proc lassoc {ls key args} {
  foreach {k v} $ls {if {$k eq $key} {return $v}}
  foreach def $args {return $def}
  error "key $key not found"
}

# Like join, but produces a list rather than a string
proc ljoin {ls v} {
  if {[llength $ls] < 2} {return $ls}
  set res [lrange $ls 0 0]
  for {set i 1} {$i < [llength $ls]} {incr i} {
    lappend res $v
    lappend res [lindex $ls $i]
  }
  return $res
}

proc lflatmap {args} {
  return [concat {*}[uplevel 1 [list lmap {*}$args]]]
}

proc lfind {varName list body {default ""}} {
  upvar $varName var
  foreach var $list {
    set res [uplevel 1 $body]
    if {$res ne ""} {return $res}
  }
  return $default
}

proc iota {count {start 0} {step 1}} {
  set ls {}
  for {set i 0} {$i < $count} {incr i} {
    lappend ls $start
    incr start $step
  }
  return $ls
}

# Used by different kinds of tagged data
proc new_obj {type val} {return [list {type val} $type $val]}
proc type {obj} {return [: $obj type]}
proc val {obj} {return [: $obj val]}
proc assert_type {t obj} {
  if {[type $obj] ne $t} {
    error "expected $t, got object of type [type $obj]"
  }
}

proc import_alias {target_ns args} {
  set source_ns [uplevel 1 {namespace current}]
  foreach cmd $args {interp alias {} ${source_ns}::${cmd} {} ${target_ns}::${cmd}}
}

# Not sure if robust
proc js_escape_string {str} {
  return [string map {"\"" "\\\"" "'" "\\'" "\\" "\\\\"} $str]
}

proc ordinal {n} {
  set n [remove_leading_zeros $n]
  regexp {1?\d$} $n s
  return $n[tree getor {1 st 2 nd 3 rd} $s th]
}

proc oxford {ls} {
  if {[llength $ls] > 1} {lset ls end "or [lindex $ls end]"}
  return [join $ls ", "]
}

proc damt {dollar cent} {
  assert {[is_int $dollar] && [is_nonneg_int $cent] && $cent < 100}
  return [list {dollar cent} $dollar $cent]
}
proc damt_dollars {v} {return [: $v dollar]}
proc damt_cents {v} {return [: $v cent]}
proc damt_format {v} {
  set c [damt_cents $v]
  if {$c < 10} {set c "0$c"}
  return [damt_dollars $v].$c
}
proc damt_in_cents {v} {return [expr {[damt_dollars $v]*100+[damt_cents $v]}]}
proc damt_from_cents {c} {return [list {dollar cent} [expr {$c/100}] [expr {$c%100}]]}
proc damt_parse {v} {
  if {[regex::match {^\$(\d+)\.(\d\d)$} $v -> d c]} {return [damt $d [remove_leading_zeros $c]]}
  return ""
}
proc damt_diff {a b} {
  return [damt_from_cents [expr {[damt_in_cents $a]-[damt_in_cents $b]}]]
}

# This is only used for Tahoe-LAFS, so a few caveats:
#  - No filename field in content disposition. Tahoe uses "name" parameter.
#  - Does not check part names. Can easily create malformed MIME messages.
#  - No way to specify content type. Everything is binary.
proc write_multlipart_form_data {chan data} {
  set boundary [hex [random_bytes 20]]
  set ls {}
  set size 0
  
  if {[tree size $data] == 0} {error "empty data"}
  tree for {k v} $data {
    lappend ls "\r\n--$boundary\r\n"
    lappend ls "Content-Disposition: form-data; name=\"$k\"\r\n"
    lappend ls "Content-Type: application/octet-stream\r\n"
    lappend ls "Content-Transfer-Encoding: binary\r\n\r\n"
    lappend ls $v
  }
  lappend ls "\r\n--$boundary--\r\n"

  foreach x $ls {
    incr size [string length $x]
  }

  fconfigure $chan -encoding binary
  puts -nonewline $chan "MIME-Version: 1.0\r\n"
  puts -nonewline $chan "Content-Type: multipart/mixed; boundary=$boundary\r\n"
  puts -nonewline $chan "Content-Length: $size\r\n\r\n"
  foreach x $ls {
    puts -nonewline $chan $x
  }
}

proc db_get {q} {
  set r [uplevel 1 [list db eval $q]]
  if {[llength $r] == 0} {error "no result"}
  return [lindex $r 0]
}

proc sql_gather {q} {
  uplevel 1 {set _r {}}
  uplevel 1 [list db eval $q _a {
    set _d {}
    foreach {_n _v} [array get _a] {
      if {$_n ne "*"} {tree set _d $_n $_v}
    }
    lappend _r $_d
  }]
  uplevel 1 {unset _a}
  return [uplevel 1 [list kill _r]]
}

proc do_async {ls} {
  set rem [llength $ls]
  set res [lrepeat $rem ""]
  set i -1
  foreach x $ls {
    {*}$x [list apply {{coro i args} {$coro [list $i {*}$args]}} \
               [info coroutine] [incr i]]
  }
  while {$rem > 0} {
    set r [lassign [yield] i]
    lset res $i $r
    incr rem -1
  }
  return $res
}

proc def_bitset {proc_name bitlist} {
  uplevel 1 [list interp alias {} $proc_name {} ::bitset $bitlist]
}

# Short name for commonly-used procedures
interp alias {} % {} html::construct
interp alias {} te {} types::eval

# Move me
proc lookup_host {host} {
  lindex [split [reader::read_all [open "|./dnsip $host" r]] " "] 0
}

# via mitmproxy
proc orig_dst {sock} {
  # From <linux/netfilter_ipv4.h>, SO_ORIGINAL_DST == 80
  set bin [posix::getsockopt $sock $::posix::SOL_IP 80 16]
  assert {[string length $bin] == 16}
  binary scan $bin ttucu4c8 _ port ip _
  return [list [join $ip .] $port]
}
