(module
  ;; Import atmosphere functions from host
  (import "atmo" "log" (func $atmo_log (param i32 i32)))

  ;; Export memory so host can write event data into it
  (memory (export "memory") 1)

  ;; on_event: called by host with pointer and length of event JSON
  ;; Simply echoes the event back via atmo_log
  (func (export "on_event") (param $ptr i32) (param $len i32)
    local.get $ptr
    local.get $len
    call $atmo_log
  )
)
