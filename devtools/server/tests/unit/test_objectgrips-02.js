/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable no-shadow, max-nested-callbacks */

"use strict";

Services.prefs.setBoolPref("security.allow_eval_with_system_principal", true);
registerCleanupFunction(() => {
  Services.prefs.clearUserPref("security.allow_eval_with_system_principal");
});

add_task(
  threadClientTest(async ({ threadClient, debuggee, client }) => {
    return new Promise(resolve => {
      threadClient.addOneTimeListener("paused", function(event, packet) {
        const args = packet.frame.arguments;

        Assert.equal(args[0].class, "Object");

        const objClient = threadClient.pauseGrip(args[0]);
        objClient.getPrototype(function(response) {
          Assert.ok(response.prototype != undefined);

          const protoClient = threadClient.pauseGrip(response.prototype);
          protoClient.getOwnPropertyNames(function(response) {
            Assert.equal(response.ownPropertyNames.length, 2);
            Assert.equal(response.ownPropertyNames[0], "b");
            Assert.equal(response.ownPropertyNames[1], "c");

            threadClient.resume().then(resolve);
          });
        });
      });

      debuggee.eval(
        function stopMe(arg1) {
          debugger;
        }.toString()
      );
      debuggee.eval(
        function Constr() {
          this.a = 1;
        }.toString()
      );
      debuggee.eval(
        "Constr.prototype = { b: true, c: 'foo' }; var o = new Constr(); stopMe(o)"
      );
    });
  })
);
