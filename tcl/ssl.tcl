proc ssl::handshake {chan {timeout -1}} {
  set alarm [if {$timeout != -1} {
    after [expr {$timeout*1000}] [list [info coroutine] handshake_timeout]
  }]
  try {
    while {[set ev [ssl::_continue_handshake $chan]] ne "done"} {
      fileevent $chan $ev [list [info coroutine] handshake_ready]
      if {[yield] eq "handshake_timeout"} {set alarm ""; error timeout}
      fileevent $chan $ev ""
    }
  } finally {
    if {$alarm ne ""} {after cancel $alarm}
  }
}
