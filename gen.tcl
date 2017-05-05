source tcl/util.tcl
source tcl/adt.tcl
source tcl/tcl_parser.tcl
source tcl/parse.tcl
source tcl/html.tcl
source tcl/regex.tcl
source tcl/types.tcl

proc cmd_usage {ls} {
  % {(table boder=0
      {*}${lmap {cmd desc} $ls {
        if {$cmd eq "--"} {
          % {(tr (th colspan=2 style={text-align:left} "$desc"))}
        } else {
          % {(tr
              (td style={background:#dcdcdc} {*}${% $cmd})
              (td {*}${% $desc}))}
        }
      }})}
}

proc page {name} {
  set chan [open pages/$name]
  close_eventually chan
  set title [gets $chan]
  set body [% [read $chan]]
  write_file www/$name.html [html::to_string [% {
    ${list literal {<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01//EN"
      "http://www.w3.org/TR/html4/strict.dtd">}}
    (html
     (head
      (title "$title")
      (meta http-equiv={content-type} content={text/html; charset=utf-8}))
     (body
      {*}${
        if {$name ne "index"} {% {(a href={index.html} {[Back to Tcl Stuff]})}}
      }
      (h1 "$title")
      {*}$body))
  }]]
}

foreach f [glob c/*.c tcl/*.tcl] {
  file copy -force $f www/[file tail $f].txt
}

foreach f [glob pages/*] {
  if {[string index $f end] eq "~"} {continue}
  page [file tail $f]
}
