package me.arnee.waddle

import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * The mesh node: the same C++ code as on the T5 (src/services/mesh), through
 * waddle_jni.cpp. Every call goes through [run], on one thread.
 */
object Mesh {
    init {
        System.loadLibrary("waddle")
    }

    private val thread: ExecutorService = Executors.newSingleThreadExecutor()

    /** Runs [work] on the mesh thread. */
    fun run(work: () -> Unit) {
        thread.execute(work)
    }

    /** Loads the node from the app's files: {"configured", "me", "name"}. */
    @JvmStatic external fun start(dir: String): String
    @JvmStatic external fun changes(): Int
    /** [{"conv", "title", "unread", "time", "last", "group"}] */
    @JvmStatic external fun conversations(): String
    /** [{"mine", "from", "text", "time", "warning", "state"}], a poll or game also
     *  {"sender", "counter", "poll": {"question", "options": [{"text", "count", "mine", "names"}]}}
     *  or {..., "game": {"status", "canMove", "board", "players"}}; votes and moves are left out. */
    @JvmStatic external fun chat(conv: Int): String
    @JvmStatic external fun title(conv: Int): String
    @JvmStatic external fun canWrite(conv: Int): Boolean
    @JvmStatic external fun send(conv: Int, text: String): Boolean
    @JvmStatic external fun markRead(conv: Int)

    /** [{"number", "name"}]: the others in the network. */
    @JvmStatic external fun members(): String
    /** A group of me and these members (bit n-1 = member n): its conversation, or -1. */
    @JvmStatic external fun createGroup(members: Long, name: String): Int
    /** {"members": [names], "left"} */
    @JvmStatic external fun group(conv: Int): String
    @JvmStatic external fun leaveGroup(conv: Int)

    /** The body for the Pi's POST /api/packets. */
    @JvmStatic external fun syncRequest(): ByteArray
    /** The Pi's answer and its Date (unix seconds); returns how many packets. */
    @JvmStatic external fun syncAnswer(answer: ByteArray, date: Long): Int
    /** {"url", "token", "cert"} */
    @JvmStatic external fun server(): String

    /** Pairing with the T5 (PairActivity). */
    @JvmStatic external fun pairBegin(): String
    @JvmStatic external fun pairAnswer(publicKey: String, nonce: String): String
    @JvmStatic external fun pairRequest(): String
    /** "" when paired, else what went wrong. */
    @JvmStatic external fun pairFinish(box: String): String

    /** Polls and Fire på rad (lib/WaddleMesh MeshPlay.h); options one per line. */
    @JvmStatic external fun startPoll(conv: Int, question: String, options: String): Boolean
    @JvmStatic external fun vote(conv: Int, sender: Int, counter: Long, option: Int): Boolean
    @JvmStatic external fun startGame(conv: Int): Boolean
    /** Drops my piece in column [col] (0-6) of the game, when it is my move. */
    @JvmStatic external fun move(conv: Int, sender: Int, counter: Long, col: Int): Boolean
}
