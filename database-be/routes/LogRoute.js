const express = require("express");
const logRepo = require("../repositories/log.repository");
const router = express.Router();

// add log
router.post("/", logRepo.addLog);

// get all logs
router.get("/", logRepo.getLog);

// get logs by userId
router.get("/:userId", logRepo.getLogByUserId);

// delete log
router.delete("/:logId", logRepo.deleteLog);

module.exports = router;
