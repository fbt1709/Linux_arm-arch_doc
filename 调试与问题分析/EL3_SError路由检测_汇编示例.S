/*
 * EL3 SError路由检测 - 汇编版本
 * 
 * 在 runtime_exceptions.S 的 serror_aarch64 入口处添加以下代码
 */

vector_entry serror_aarch64
#if FFH_SUPPORT
    save_x30
    apply_at_speculative_wa
    
    /* ========== 添加以下检测代码 ========== */
    /* 读取SCR_EL3并检查EA位 */
    mrs     x0, scr_el3
    tst     x0, #SCR_EA_BIT
    b.eq    serror_routed_to_lower_el
    
    /* SError路由到EL3 (FFH) */
    adr     x0, serror_ffh_msg
    bl      asm_print_str
    b       serror_check_done
    
serror_routed_to_lower_el:
    /* SError路由到低异常等级 (KFH) */
    adr     x0, serror_kfh_msg
    bl      asm_print_str
    
serror_check_done:
    /* 打印SCR_EL3和ESR_EL3的值 */
    adr     x0, scr_el3_msg
    bl      asm_print_str
    mrs     x0, scr_el3
    bl      asm_print_hex
    
    adr     x0, esr_el3_msg
    bl      asm_print_str
    mrs     x0, esr_el3
    bl      asm_print_hex
    /* ========== 检测代码结束 ========== */
    
    sync_and_handle_pending_serror
    b       handle_lower_el_async_ea
#else
    b       report_unhandled_exception
#endif
end_vector_entry serror_aarch64

/* 字符串定义 */
serror_ffh_msg:
    .asciz "[EL3] SError路由到EL3 (FFH)\n"
serror_kfh_msg:
    .asciz "[EL3] SError路由到低异常等级 (KFH)\n"
scr_el3_msg:
    .asciz "SCR_EL3 = 0x"
esr_el3_msg:
    .asciz ", ESR_EL3 = 0x"

