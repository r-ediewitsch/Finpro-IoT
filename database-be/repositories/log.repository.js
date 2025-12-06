const Log = require("../models/LogModel");

async function addLog(req, res) {
    try {
        const { userId, room, timestamp } = req.body;

        if (!userId || !room) {
            throw new Error("userId and room are required");
        }

        const log = new Log({ userId, room, timestamp: timestamp || Date.now() });
        await log.save();

        res.status(200).json({ 
            success: true, 
            message: "Successfully added log", 
            data: log 
        });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

async function getLog(req, res) {
    try {
        const logs = await Log.find();
        res.status(200).json({ success: true, message: "Found all logs", data: logs });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

async function getLogByUserId(req, res) {
    try {
        const userId = req.params.userId;
        const logs = await Log.find({ userId });
        
        if (logs.length === 0) throw new Error("No logs found for this user");
        
        res.status(200).json({ success: true, message: "Found logs for user", data: logs });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

async function deleteLog(req, res) {
    try {
        const log = await Log.findByIdAndDelete(req.params.logId);
        if (!log) throw new Error("Log not found");

        res.status(200).json({ success: true, message: "Successfully deleted log" });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

module.exports = {
    addLog,
    getLog,
    getLogByUserId,
    deleteLog
};
