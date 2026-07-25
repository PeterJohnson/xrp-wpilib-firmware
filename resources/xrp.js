window.onload = () => {

    console.log("hello");
    const configJsonEntry = document.getElementById("configJson");
    const resetButton = document.getElementById("resetButton");
    const saveButton = document.getElementById("saveButton");
    
    // Hook up button events
    resetButton.onclick = (e) => {
        e.preventDefault();
        console.log("Reset Button Clicked");
        fetch("/resetconfig", {
            method: "post"
        })
        .then(() => {
            loadConfig()
            .then(() => {
                alert("Configuration reset to default. Please reset the XRP");
            });
        });
    };
    
    saveButton.onclick = (e) => {
        e.preventDefault();
        console.log("Save Button Clicked");
        
        try {
            // Check if can parse
            let jsonObj = JSON.parse(configJsonEntry.value);
            
            if(!jsonObj["bluetooth"] || typeof jsonObj["bluetooth"]["deviceName"] !== "string") {
                throw new Error("In \"bluetooth\", must have field \"deviceName\": \"WPIXRP-name\"");
            }

            let deviceName = jsonObj["bluetooth"]["deviceName"];
            if(deviceName.length < 1 || deviceName.length > 29) {
                throw new Error("Bluetooth device name must be between 1 and 29 characters");
            }

            for(let i = 0; i < deviceName.length; i++) {
                let code = deviceName.charCodeAt(i);
                if(code < 0x20 || code > 0x7e) {
                    throw new Error("Bluetooth device name must use printable ASCII characters");
                }
            }

            // Don't allow for escape characters
            if(configJsonEntry.value.toString().includes("\\")) {
                throw new Error("Cannot use escape character '\\'.");
            }

            // Save config back to XRP disk
            fetch("/saveconfig", {
                body: configJsonEntry.value,
                headers: {
                    "Content-Type": "text/json"
                },
                method: "post"
            })
            .then(() => {
                alert("Configuration Updated. Please reset the XRP");
            });
        }
        catch (e) {
            alert("Invalid JSON. Please check and try again.\n" + e.message);
        }
    }
    
    // load data and then enable the buttons
    loadConfig();
    
    function loadConfig() {
        return fetch("/getconfig")
        .then((response) => response.json())
        .then((xrpConfigJson) => {
            configJsonEntry.value = JSON.stringify(xrpConfigJson, null, 4);
        })
        .catch((err) => {
            console.log("ERROR");
            console.log(err);
        });
    }
    
    };
