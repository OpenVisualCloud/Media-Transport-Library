import yaml


def load_ebu_config(config_path: str):
    with open(config_path, "r") as f:
        config = yaml.safe_load(f)
    instance = config["instances"]
    config_response = {
        "ebu_ip": instance.get("name", ""),
        "username": instance.get("username", ""),
        "password": instance.get("password", "")
    }
    return config_response
