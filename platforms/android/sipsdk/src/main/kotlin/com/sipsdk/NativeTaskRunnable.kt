package com.sipsdk

internal class NativeTaskRunnable(
    private val taskPtr: Long,
    private val argPtr: Long
) : Runnable {
    override fun run() {
        nativeRun(taskPtr, argPtr)
    }

    private external fun nativeRun(task: Long, arg: Long)
}
