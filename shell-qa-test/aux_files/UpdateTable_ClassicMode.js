var mysql=require('mysql').mysql;
var session=mysql.getClassicSession('root:guidev!@localhost:3578');
session.runSql('use sakila;');
session.runSql("UPDATE actor SET first_name = 'Updated By JS Classic STDIN' where actor_id = 50 ;");

session.runSql("SELECT * FROM sakila.actor where actor_id = 50;");
session.runSql("UPDATE actor SET first_name = 'Old value' where actor_id = 50 ;");