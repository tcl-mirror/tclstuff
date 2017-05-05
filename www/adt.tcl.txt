namespace eval adt {}

proc adt::define {cmd labels {parent ""} {props ""}} {
  set id [gen_id]
  set body "set adt \[force \$$id\]"
  append body {
    return [apply [tree getcache {
      spec {{adt} {return $adt}}
      map {{adt value fun args} {return [adt::map $adt $value $fun {*}$args]}}
      map_shape {{adt value post {pre id}} {
        return [adt::map_shape $adt this $value $post $pre]
      }}
      get_prop {{adt val prop {default ""}} {
        return [adt::get_prop $adt $val $prop $default]
      }}
      set_prop {{adt var_name prop prop_val} {
        upvar 2 $var_name val
        return [adt::set_prop $adt val $prop $prop_val]
      }}
      encode {{adt value} {return [adt::encode $adt $value]}}
      decode {{adt value} {return [adt::decode $adt $value]}}
    } $cmd] $adt {*}$args]
  }
  set $id [list adt::create $labels $parent $props]; # delay
  uplevel 1 [list proc $cmd {cmd args} $body]
}

proc adt::create {labels parent {props ""}} {
  set props [parse::parse_string {[fcomma_sep ~(ident ,':' adt::parse_val_spec)]} $props]
  set labels [parse::parse_string {
    (ident [paren [fcomma_sep ~(ident ,':' adt::parse_val_spec)]]){*}
  } $labels]
  return [list {labels parent props} $labels $parent $props]
}

# Recurse on value spec (this, any, int, etc).
proc adt::map_shape {adt spec value post {pre id}} {
  switch [lindex $spec 0] {
    this {
      set label [lindex $value 0]
      set field_specs [get_field_specs $adt $label]
      set value [{*}$pre [kill value]]

      # Update fields
      set values [list $label]
      set i 0
      foreach {field_name field_spec} $field_specs {
        lappend values [map_shape $adt $field_spec [lindex $value [incr i]] $post $pre]
      }

      set props {}
      tree for {k v} [lindex $value $i+1] {
        tree set props $k [map_shape $adt [get_field_specs $adt $k props] $v $post $pre]
      }
      if {[tree size $props] > 0} {lappend values $props}

      if {[llength $post] == 0} {return $values}
      return [{*}$post this $values]
    }
    tuple {
      set elt_specs [lindex $spec 1]
      if {[llength $value] != [llength $elt_specs]} {error ""}
      set ls {}
      foreach elt $value elt_spec $elt_specs {
        lappend ls [map_shape $adt $elt_spec $elt $post $pre]
      }
      if {[llength $post] == 0} {return $ls}
      return [{*}$post tuple $ls]
    }
    list {
      set elt_specs [lindex $spec 1]
      if {([llength $value] % [llength $elt_specs]) != 0} {error ""}
      set vars {}
      foreach x $elt_specs {lappend vars v[incr i]}
      set ls {}
      foreach $vars $value {
        foreach var_name $vars elt_spec $elt_specs {
          lappend ls [map_shape $adt $elt_spec [set $var_name] $post $pre]
        }
      }
      if {[llength $post] == 0} {return $ls}
      return [{*}$post list $ls]
    }
    dict {
      set d {}
      tree for {k v} $value {
        tree set d $k [map_shape $adt [lindex $spec 2] $v $post $pre]
      }
      if {[llength $post] == 0} {return $d}
      return [{*}$post dict $d]
    }
    default {
      if {[llength $post] == 0} {return $value}
      return [{*}$post [lindex $spec 0] $value]
    }
  }
}

proc adt::get_field_specs {adt name {kind labels}} {
  # Find label spec
  set spec [tree getcache* [: $adt $kind] $name]
  while {[llength $spec] == 0} {
    if {[llength [force [: $adt parent]]] == 0} {error "unknown [regsub {s$} $kind ""] $name"}
    set adt [force [: $adt parent]]
    set spec [tree getcache* [: $adt $kind] $name]
  }
  return [lindex $spec 0]
}

# Recurse case-by-case on labels
proc adt::map {adt value fun args} {
  # All variables are prefixed with underscore because procedure
  # activations double for map case handlers.
  set f [list apply {{_fun _vars _kind _value} {
    if {$_kind ne "this"} {return $_value}
    upvar 1 field_specs _field_specs
    set _label [lindex $_value 0]

    # Assign all field values to variables. This is why we need to
    # prefix all of the locals in this function.
    set _i 0
    foreach {_field_name _field_spec} $_field_specs {
      set $_field_name [lindex $_value [incr _i]]
    }

    # Assign all properties to variables. TODO: take out? Access
    # directly? Access with @prop? Need to decide on best interface.
    set _props [lindex $_value $_i+1]

    # Additional arguments passed to map
    foreach {_var_name _var_value} $_vars {
      set $_var_name $_var_value
    }

    # Find handler for label
    lassign $_fun _inner _super
    set _handler [tree get* $_inner $_label]
    while {[llength $_handler] == 0} {
      foreach _h [tree get* $_inner *default*] {return [eval $_h]}
      if {[llength $_super] == 0} {return $_value}
      lassign $_super _inner _super
      set _handler [tree get* $_inner $_label]
    }

    return [eval [lindex $_handler 0]]
  }} $fun $args]

  return [map_shape $adt this $value $f]
}

# Special functions for use inside handler passed to map to access
# _-prefixed special variables. Maybe access directly instead?
proc adt::@prop_map {} {uplevel 1 [set _props]}
proc adt::@prop {prop {default ""}} {
  upvar 1 _props props
  return [tree getor $props $prop $default]
}
proc adt::@set_prop {prop prop_val} {
  upvar 1 _props props
  tree set props $prop $prop_val
}
proc adt::@update {args} {
  upvar 1 _value value
  upvar 1 _props props
  return [list [lindex $_value 0] {*}$args $_props]
}

proc adt::prop_map_index {adt val} {
  return [expr {1+([llength [get_field_specs $adt [lindex $val 0]]] / 2)}]
}
proc adt::get_prop {adt val prop {default ""}} {
  return [tree getor [lindex $val [prop_map_index $adt $val]] $prop $default]
}
proc adt::set_prop {adt var_name prop prop_val} {
  upvar 1 $var_name val
  set index [prop_map_index $adt $val]
  lset val $index [tree replace [lindex $val $index] $prop $prop_val]
}

proc adt::parse_val_spec {in_var} {
  upvar 1 $in_var in
  parse::exp {
    [paren [comma_sep adt::parse_val_spec]] >> {list tuple $x}
    | [bracket [comma_sep adt::parse_val_spec]] >> {list list $x}
    | [brace (adt::parse_val_spec ~('->' ^adt::parse_val_spec)?)] >> {
      lassign $x from to
      if {[llength $to] == 0} {
        set to $from
        set from string
      } else {
        set to [lindex $to 0]
      }
      return [list dict $from $to]
    }
    | '_' = {any}
    | '*' = {this}
    | ident
  } in
}

proc adt::encode {adt value} {
  set f {apply {{kind value} {
    switch $kind {
      this {
        upvar 1 field_specs field_specs
        set d [tree create type [new_obj string [lindex $value 0]]]
        set i 0
        foreach {field_name field_spec} $field_specs {
          tree set d $field_name [lindex $value [incr i]]
        }
        tree for {k v} [lindex $value $i+1] {tree set d $k $v}
        return [new_obj dict $d]
      }
      tuple {return [new_obj list $value]}
      any {return $value}
      hidden {return [new_obj int -1]}
      default {return [new_obj $kind $value]}
    }
  }}}

  return [map_shape $adt this $value $f]
}

proc adt::decode {adt value} {
  return [decode_val $adt this $value]
}

proc adt::decode_val {adt val_spec value} {
  switch [lindex $val_spec 0] {
    this {
      set d [tagged::untag $value {{type:string}}]
      set label [tree get $d type]
      tree unset d type
      set field_specs [get_field_specs $adt $label]
      set res [list $label]
      foreach {field_name field_spec} $field_specs {
        lappend res [decode_val $adt $field_spec [tree get $d $field_name]]
        tree unset d $field_name
      }
      set props {}
      tree for {k v} $d {
        tree set props $k [decode_val $adt [get_field_specs $adt $k props] $v]
      }
      if {[tree size $props] > 0} {lappend res $props}
      return $res
    }
    list - tuple {
      set types [lindex $val_spec 1]
      set ls [tagged::untag $value {[]}]
      assert {
        ([lindex $val_spec 0] eq "list" && ([llength $val] % [llength $types]) == 0) ||
        ([lindex $val_spec 0] eq "tuple" && [llength $val] == [llength $types])
      }
      for {set i 0} {$i < [llength $ls]} {incr i} {
        set elt [lindex $ls $i]
        lset ls $i [decode_val $adt [lindex $types [expr {$i%[llength $types]}]] $elt]
      }
      return $ls
    }
    dict {
      lassign $val_spec _ key_type value_type
      set d {}
      tree for {k v} [tagged::untag $value {{}}] {
        tree set d $k [decode_val $adt $value_type $v]
      }
      return $d
    }
    any {
      return $value
    }
    hidden {
      error "cannot decode value of hidden type"
    }
    default {
      return [tagged::untag* $value $val_spec]
    }
  }
}
