const bcrypt = require('bcrypt');
const crypto = require('crypto');
const User = require("../models/UserModel");

async function addUser(req, res) {
    try {
        const { userId, password, role, allowedRoom } = req.body;

        if (!userId || !password || !role) {
            throw new Error("userId, password and role are required");
        }

        // Generate secretKey automatically
        const secretKey = crypto.randomBytes(32).toString('hex');

        const user = new User({ userId, secretKey, password, role, allowedRoom });
        await user.save();

        const response = user.toObject();
        delete response.password;

        res.status(200).json({ 
            success: true, 
            message: "Successfully Registered User", 
            data: response 
        });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

async function login(req, res) {
    try {
        const { userId, password } = req.body;
        
        const user = await User.findOne({ userId: userId });
        if (!user) throw new Error("User not found");

        const isMatch = await bcrypt.compare(password, user.password);
        if (!isMatch) throw new Error("Invalid Password");

        const response = user.toObject();
        delete response.password;

        res.status(200).json({ success: true, message: "Login successful", data: response });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

async function getAllUser(req, res) {
    try {
        const users = await User.find().select('-password');
        res.status(200).json({ success: true, message: "Found all users", data: users });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

async function deleteUser(req, res) {
    try {
        const user = await User.findByIdAndDelete(req.params.userId);
        if (!user) throw new Error("User not found");

        res.status(200).json({ success: true, message: "Successfully deleted user" });
    } catch (err) {
        res.status(400).json({ success: false, message: err.message });
        console.log(`Error Message: ${err.message}`);
    }
}

module.exports = {
    addUser,
    login,
    getAllUser,
    deleteUser
};