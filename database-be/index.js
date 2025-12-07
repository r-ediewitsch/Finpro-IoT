require("dotenv").config();

const express = require('express');
const cors = require('cors');
const db = require('./config/db');

const userRoutes = require('./routes/UserRoute');
const logRoutes = require('./routes/LogRoute');

const app = express();

// Connect to database
db.connectDB();

// Middlewares
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(cors());

// Routes
app.use("/user", userRoutes);
app.use("/log", logRoutes);

// Health check endpoint
app.get("/", (req, res) => {
    res.status(200).json({ success: true, message: "Server is running" });
});

// Error handling middleware
app.use((err, req, res, next) => {
    console.error(`Error: ${err.message}`);
    res.status(500).json({ success: false, message: err.message });
});

const PORT = process.env.PORT || 5000;

// ONLY listen if running locally
if (require.main === module) {
    app.listen(PORT, () => {
        console.log(`🚀 Server running on port ${PORT}`);
    });
}

// Export the app for Vercel
module.exports = app;