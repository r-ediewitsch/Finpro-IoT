const mongoose = require('mongoose');

const logSchema = new mongoose.Schema(
    {
        userId: {
            type: String,
            required: true,
        },
        room: {
            type: String,
            required: true,
        },
        timestamp: {
            type: Date,
            required: true,
            default: Date.now,
        },
    }, { timestamps: true }
);

const Log = mongoose.model("Log", logSchema);

module.exports = Log;
