require("dotenv").config();

const userRoutes = require('../routes/UserRoute');
const logRoutes = require('../routes/LogRoute');

const express = require('express');
const cors = require('cors');
const db = require('../config/db');

const app = express();

// connect to database
db.connectDB();

// middlewares
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(cors());

app.use("/user", userRoutes);
app.use("/log", logRoutes);

module.exports = app;