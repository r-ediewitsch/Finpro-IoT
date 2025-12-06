const express = require("express");
const userRepo = require("../repositories/user.repository");
const router = express.Router();

// get all users
router.get("/", userRepo.getAllUser);

// login
router.post("/login", userRepo.login);

// add user
router.post("/register", userRepo.addUser);

// delete user
router.delete("/:userId", userRepo.deleteUser);

module.exports = router;