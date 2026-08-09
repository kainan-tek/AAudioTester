package com.example.aaudiotester.common

/** 各特性的状态文案。 */
data class AAudioMessages(
    val ready: String,
    val preparing: String,
    val active: String,
    val stopped: String,
    val failed: String,
)
